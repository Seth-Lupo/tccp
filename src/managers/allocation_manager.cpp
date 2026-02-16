#include "allocation_manager.hpp"
#include "gpu_discovery.hpp"
#include <core/config.hpp>
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <core/resource_spec.hpp>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <set>
#include <ctime>

AllocationManager::AllocationManager(const Config& config, SSHConnection& dtn,
                                     SSHConnection& login, StateStore& store)
    : config_(config), dtn_(dtn), login_(login), store_(store),
      username_(get_cluster_username()) {
}

std::string AllocationManager::persistent_base() const {
    return fmt::format(REMOTE_PROJECT_BASE, username_, config_.project().name);
}

std::string AllocationManager::container_cache() const {
    return fmt::format(REMOTE_CONTAINER_CACHE, username_);
}

void AllocationManager::persist() {
    store_.save(state_);
}

int AllocationManager::parse_time_minutes(const std::string& time_str) {
    if (time_str.empty()) return DEFAULT_ALLOC_MINUTES;

    int h = 0, m = 0, s = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d", &h, &m, &s) >= 2) {
        return h * 60 + m + (s > 0 ? 1 : 0);
    }
    return DEFAULT_ALLOC_MINUTES;
}

void AllocationManager::reconcile(StatusCallback cb) {
    state_ = store_.load();

    if (state_.allocations.empty()) {
        if (cb) cb("No previous allocations found");
        return;
    }

    if (cb) cb(fmt::format("Reconciling {} allocation(s) with SLURM...",
                           state_.allocations.size()));

    // Build set of job IDs that are still active (not completed/canceled)
    std::set<std::string> active_jobs;
    for (const auto& js : state_.jobs) {
        if (!js.completed && !js.canceled && js.init_error.empty()) {
            active_jobs.insert(js.job_id);
        }
    }

    std::vector<AllocationState> live;
    for (auto& alloc : state_.allocations) {
        auto result = query_alloc_slurm(alloc.slurm_id);

        if (result.state == "RUNNING") {
            if (!result.node.empty()) alloc.node = result.node;
            // Clear stale active_job_id if that job is no longer active
            if (!alloc.active_job_id.empty() &&
                active_jobs.find(alloc.active_job_id) == active_jobs.end()) {
                alloc.active_job_id.clear();
            }
            live.push_back(alloc);
            if (cb) cb(fmt::format("  Allocation {} RUNNING on {}{}", alloc.slurm_id, alloc.node,
                                   alloc.active_job_id.empty() ? " (idle)" : " (busy)"));
        } else if (result.state == "PENDING") {
            live.push_back(alloc);
            if (cb) cb(fmt::format("  Allocation {} PENDING", alloc.slurm_id));
        } else {
            if (cb) cb(fmt::format("  Allocation {} gone ({})", alloc.slurm_id,
                                   result.state.empty() ? "not found" : result.state));
        }
    }

    state_.allocations = live;
    persist();
}

AllocationState* AllocationManager::claim_free(int required_minutes,
                                                const SlurmDefaults& required_resources,
                                                const std::string& job_id,
                                                StatusCallback cb) {
    std::lock_guard<std::mutex> lock(alloc_mutex_);
    auto* alloc = find_free_unlocked(required_minutes, required_resources, cb);
    if (alloc) {
        alloc->active_job_id = job_id;
        persist();
    }
    return alloc;
}

AllocationState* AllocationManager::find_free_unlocked(int required_minutes,
                                                        const SlurmDefaults& required_resources,
                                                        StatusCallback cb) {
    if (state_.allocations.empty()) {
        if (cb) cb("No existing allocations to reuse");
        return nullptr;
    }

    if (cb) cb(fmt::format("Checking {} allocation(s) (need {}min, partition={}, gpu={}:{})",
                           state_.allocations.size(), required_minutes,
                           required_resources.partition.empty() ? DEFAULT_PARTITION : required_resources.partition,
                           required_resources.gpu_type, required_resources.gpu_count));

    // First pass: clear stale active_job_ids
    clear_stale_job_refs(cb);

    // Second pass: find a compatible idle allocation
    for (auto& alloc : state_.allocations) {
        std::string id = alloc.slurm_id;

        if (!alloc.node.empty() && alloc.active_job_id.empty()) {
            int rem = remaining_minutes(alloc);
            if (rem < required_minutes) {
                if (cb) cb(fmt::format("  {} — skip: {}min remaining < {}min needed",
                                       id, rem, required_minutes));
                continue;
            }

            SlurmDefaults alloc_res = alloc_to_slurm_defaults(alloc);
            if (resources_compatible(alloc_res, required_resources)) {
                if (cb) cb(fmt::format("  {} — match! ({}min left, partition={}, gpu={}:{})",
                                       id, rem, alloc.partition, alloc.gpu_type, alloc.gpu_count));
                return &alloc;
            } else {
                if (cb) cb(fmt::format("  {} — skip: resources mismatch", id));
            }
        } else if (alloc.node.empty()) {
            if (cb) cb(fmt::format("  {} — skip: no node yet (PENDING)", id));
        } else {
            if (cb) cb(fmt::format("  {} — skip: busy (job={})", id, alloc.active_job_id));
        }
    }

    if (cb) cb("No compatible free allocation found");
    return nullptr;
}

void AllocationManager::clear_stale_job_refs(StatusCallback cb) {
    bool cleared = false;
    for (auto& alloc : state_.allocations) {
        if (alloc.active_job_id.empty()) continue;
        bool job_still_active = false;
        for (const auto& js : state_.jobs) {
            if (js.job_id == alloc.active_job_id) {
                job_still_active = !js.completed && !js.canceled && js.init_error.empty();
                break;
            }
        }
        if (!job_still_active) {
            if (cb) cb(fmt::format("  Clearing stale job ref on alloc {} (job {} done)",
                                   alloc.slurm_id, alloc.active_job_id));
            alloc.active_job_id.clear();
            cleared = true;
        }
    }
    if (cleared) persist();
}

SlurmDefaults AllocationManager::alloc_to_slurm_defaults(const AllocationState& alloc) const {
    SlurmDefaults d{};
    d.partition = alloc.partition;
    d.nodes = alloc.nodes;
    d.cpus_per_task = alloc.cpus;
    d.memory = alloc.memory;
    d.gpu_type = alloc.gpu_type;
    d.gpu_count = alloc.gpu_count;
    return d;
}

AllocationManager::SlurmQueryResult AllocationManager::query_alloc_slurm(const std::string& slurm_id) {
    SlurmQueryResult result;
    std::string cmd = fmt::format("squeue -j {} -h -o \"%T %N\"", slurm_id);

    // Retry on empty responses (transient SSH/SLURM hiccups)
    for (int attempt = 0; attempt <= SSH_CMD_MAX_RETRIES; attempt++) {
        auto qr = login_.run(cmd);
        std::istringstream iss(qr.stdout_data);
        iss >> result.state >> result.node;

        if (!result.state.empty()) return result;

        // Empty response — could be transient
        if (attempt < SSH_CMD_MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(SSH_RETRY_DELAY_SECS));
        }
    }
    return result;  // Return empty if all retries failed
}

AllocationState* AllocationManager::find_by_id(const std::string& slurm_id) {
    for (auto& alloc : state_.allocations) {
        if (alloc.slurm_id == slurm_id) {
            return &alloc;
        }
    }
    return nullptr;
}

AllocationState* AllocationManager::find_pending(const SlurmDefaults& required_resources) {
    for (auto& alloc : state_.allocations) {
        if (alloc.active_job_id.empty() && alloc.node.empty()) {
            SlurmDefaults alloc_res = alloc_to_slurm_defaults(alloc);
            if (resources_compatible(alloc_res, required_resources)) {
                return &alloc;
            }
        }
    }
    return nullptr;
}

// ── wait_for_allocation (decomposed) ────────────────────────

void AllocationManager::remove_allocation(const std::string& slurm_id) {
    auto& allocs = state_.allocations;
    allocs.erase(
        std::remove_if(allocs.begin(), allocs.end(),
            [&](const AllocationState& a) { return a.slurm_id == slurm_id; }),
        allocs.end());
    persist();
}

AllocationManager::WaitOutcome AllocationManager::check_alloc_state(
        const std::string& slurm_id, StatusCallback cb) {
    auto qr = query_alloc_slurm(slurm_id);

    if (qr.state == "RUNNING" && !qr.node.empty()) {
        // Update persisted allocation with node info
        for (auto& alloc : state_.allocations) {
            if (alloc.slurm_id == slurm_id) {
                alloc.node = qr.node;
                alloc.start_time = now_iso();
                persist();
                if (cb) cb(fmt::format("Allocation {} running on {}", slurm_id, qr.node));
                return {WaitOutcome::RUNNING, alloc, ""};
            }
        }
    }

    if (qr.state == "COMPLETED" || qr.state == "FAILED" || qr.state == "CANCELLED") {
        remove_allocation(slurm_id);
        return {WaitOutcome::DEAD, {}, fmt::format("Allocation {} died: {}", slurm_id, qr.state)};
    }

    if (qr.state.empty()) {
        // Could be transient or truly gone — query_alloc_slurm already retried
        remove_allocation(slurm_id);
        return {WaitOutcome::DEAD, {},
                fmt::format("Lost contact with allocation {} (SLURM not responding)", slurm_id)};
    }

    // Still PENDING
    return {WaitOutcome::PENDING, {}, ""};
}

Result<AllocationState> AllocationManager::wait_for_allocation(
        const std::string& slurm_id, StatusCallback cb) {
    for (int i = 0; i < ALLOC_WAIT_MAX_ITERS; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(ALLOC_WAIT_POLL_SECS));

        auto outcome = check_alloc_state(slurm_id, cb);

        switch (outcome.status) {
            case WaitOutcome::RUNNING:
                return Result<AllocationState>::Ok(outcome.alloc);
            case WaitOutcome::DEAD:
                return Result<AllocationState>::Err(outcome.error);
            case WaitOutcome::PENDING:
                if (i > 0 && i % ALLOC_WAIT_REPORT_INTERVAL == 0 && cb) {
                    cb(fmt::format("Still waiting for allocation {}... ({}s)",
                                   slurm_id, (i + 1) * ALLOC_WAIT_POLL_SECS));
                }
                break;
        }
    }

    // Timed out — cancel and remove
    login_.run("scancel " + slurm_id);
    remove_allocation(slurm_id);
    return Result<AllocationState>::Err(
        fmt::format("Timed out waiting for allocation {} ({}s)",
                    slurm_id, ALLOC_WAIT_MAX_ITERS * ALLOC_WAIT_POLL_SECS));
}

int AllocationManager::remaining_minutes(const AllocationState& alloc) const {
    if (alloc.start_time.empty()) return 0;

    auto start = parse_iso_time(alloc.start_time);
    if (start == 0) return 0;

    auto now = std::time(nullptr);
    int elapsed = static_cast<int>(std::difftime(now, start) / 60.0);
    int remaining = alloc.duration_minutes - elapsed;
    return remaining > 0 ? remaining : 0;
}

SlurmDefaults AllocationManager::resolve_profile(const std::string& job_name) const {
    // Start with global defaults
    SlurmDefaults profile = config_.slurm();

    // Override with project-level settings if present
    const auto& proj = config_.project();
    if (proj.slurm.has_value()) {
        const auto& ps = proj.slurm.value();
        if (!ps.partition.empty()) profile.partition = ps.partition;
        if (!ps.time.empty()) profile.time = ps.time;
        if (ps.nodes > 0) profile.nodes = ps.nodes;
        if (ps.cpus_per_task > 0) profile.cpus_per_task = ps.cpus_per_task;
        if (!ps.memory.empty()) profile.memory = ps.memory;
        if (!ps.gpu_type.empty()) profile.gpu_type = ps.gpu_type;
        if (ps.gpu_count > 0) profile.gpu_count = ps.gpu_count;
        if (!ps.exclude_nodes.empty()) profile.exclude_nodes = ps.exclude_nodes;
    }

    // Override with job-specific settings if present
    auto it = proj.jobs.find(job_name);
    if (it != proj.jobs.end() && it->second.slurm.has_value()) {
        const auto& js = it->second.slurm.value();
        if (!js.partition.empty()) profile.partition = js.partition;
        if (!js.time.empty()) profile.time = js.time;
        if (js.nodes > 0) profile.nodes = js.nodes;
        if (js.cpus_per_task > 0) profile.cpus_per_task = js.cpus_per_task;
        if (!js.memory.empty()) profile.memory = js.memory;
        if (!js.gpu_type.empty()) profile.gpu_type = js.gpu_type;
        if (js.gpu_count > 0) profile.gpu_count = js.gpu_count;
        if (!js.exclude_nodes.empty()) profile.exclude_nodes = js.exclude_nodes;
    }

    if (profile.time.empty()) {
        profile.time = DEFAULT_ALLOC_TIME;
    }

    return profile;
}

std::string AllocationManager::generate_alloc_script(const SlurmDefaults& profile) const {
    std::string base = persistent_base();

    std::string s;
    s += "#!/bin/bash\n";
    s += fmt::format("#SBATCH --job-name=tccp-{}\n", config_.project().name);
    s += fmt::format("#SBATCH --time={}\n", profile.time);
    s += generate_sbatch_resources(profile);
    s += fmt::format("#SBATCH --output={}/alloc-%j.out\n", base);
    s += fmt::format("#SBATCH --error={}/alloc-%j.err\n", base);
    s += "\nsleep infinity\n";

    return s;
}

Result<AllocationState> AllocationManager::allocate(const SlurmDefaults& profile,
                                                     StatusCallback cb) {
    if (cb) cb("Requesting compute allocation...");

    // Auto-resolve GPU partition if needed
    SlurmDefaults resolved = profile;
    if ((resolved.gpu_count > 0 || !resolved.gpu_type.empty()) &&
        (resolved.partition.empty() || resolved.partition == DEFAULT_PARTITION)) {
        if (cb) cb("Discovering GPU resources...");
        auto match = resolve_gpu_partition(login_, username_,
                                            resolved.partition,
                                            resolved.gpu_type,
                                            resolved.gpu_count);
        if (!match.found) {
            return Result<AllocationState>::Err(
                fmt::format("No suitable GPU partition: {}", match.error));
        }
        resolved.partition = match.partition;
        if (resolved.gpu_type.empty()) {
            resolved.gpu_type = match.gpu_type;
        }
        if (!match.node_prefix.empty()) {
            resolved.node_constraint = match.node_prefix;
        }
        if (cb) cb(fmt::format("Using partition '{}' (gpu:{}:{}{})",
                               match.partition, match.gpu_type, match.gpu_per_node,
                               match.node_prefix.empty() ? "" :
                               fmt::format(" nodes={}*", match.node_prefix)));
    }

    // Ensure base directories exist
    std::string base = persistent_base();
    std::string cc = container_cache();
    dtn_.run(fmt::format("mkdir -p {} {}/env {}/images {}/cache {}/tmp",
                         base, base, cc, cc, cc));

    // Submit allocation
    std::string script_content = generate_alloc_script(resolved);
    std::string submit_cmd = "sbatch << 'TCCP_ALLOC_EOF'\n"
                             + script_content + "\nTCCP_ALLOC_EOF";
    auto result = login_.run(submit_cmd);

    // Parse SLURM job ID
    std::string slurm_id;
    auto pos = result.stdout_data.find("Submitted batch job ");
    if (pos != std::string::npos) {
        std::istringstream iss(result.stdout_data.substr(pos + 20));
        iss >> slurm_id;
    }

    if (slurm_id.empty()) {
        return Result<AllocationState>::Err(
            fmt::format("Failed to submit allocation: {}", result.get_output()));
    }

    if (cb) cb(fmt::format("Allocation {} submitted, waiting for node...", slurm_id));

    // Persist immediately so allocation survives connection drops
    AllocationState alloc;
    alloc.slurm_id = slurm_id;
    alloc.duration_minutes = parse_time_minutes(resolved.time);
    alloc.partition = resolved.partition.empty() ? DEFAULT_PARTITION : resolved.partition;
    alloc.nodes = resolved.nodes > 0 ? resolved.nodes : 1;
    alloc.cpus = resolved.cpus_per_task > 0 ? resolved.cpus_per_task : 1;
    alloc.memory = resolved.memory.empty() ? DEFAULT_MEMORY : resolved.memory;
    alloc.gpu_type = resolved.gpu_type;
    alloc.gpu_count = resolved.gpu_count;
    state_.allocations.push_back(alloc);
    persist();

    return wait_for_allocation(slurm_id, cb);
}

void AllocationManager::assign_job(const std::string& slurm_id, const std::string& job_id) {
    std::lock_guard<std::mutex> lock(alloc_mutex_);
    for (auto& alloc : state_.allocations) {
        if (alloc.slurm_id == slurm_id) {
            alloc.active_job_id = job_id;
            persist();
            return;
        }
    }
}

void AllocationManager::release_job(const std::string& slurm_id) {
    std::lock_guard<std::mutex> lock(alloc_mutex_);
    for (auto& alloc : state_.allocations) {
        if (alloc.slurm_id == slurm_id) {
            alloc.active_job_id.clear();
            persist();
            return;
        }
    }
}

void AllocationManager::deallocate(const std::string& slurm_id, StatusCallback cb) {
    login_.run("scancel " + slurm_id);
    if (cb) cb(fmt::format("Cancelled allocation {}", slurm_id));
    remove_allocation(slurm_id);
}

void AllocationManager::reap_expired_idle(const Config& config, StatusCallback cb) {
    // Find the shortest job time across all configured jobs.
    const auto& jobs = config.project().jobs;
    int min_job_minutes = DEFAULT_ALLOC_MINUTES;
    for (const auto& [name, jc] : jobs) {
        std::string jt = jc.time.empty() ? DEFAULT_JOB_TIME : jc.time;
        int m = parse_time_minutes(jt);
        if (m < min_job_minutes) min_job_minutes = m;
    }

    // Cancel idle allocations that can't fit even the shortest job.
    std::vector<std::string> to_cancel;
    for (const auto& alloc : state_.allocations) {
        if (!alloc.active_job_id.empty()) continue;  // busy
        if (alloc.node.empty()) continue;             // still pending
        int rem = remaining_minutes(alloc);
        if (rem < min_job_minutes) {
            if (cb) cb(fmt::format("Releasing allocation {} — {}min left < {}min needed by shortest job",
                                   alloc.slurm_id, rem, min_job_minutes));
            to_cancel.push_back(alloc.slurm_id);
        }
    }

    for (const auto& id : to_cancel) {
        deallocate(id, nullptr);
    }
}

void AllocationManager::deallocate_all_idle(StatusCallback cb) {
    std::vector<std::string> to_cancel;
    for (const auto& alloc : state_.allocations) {
        if (alloc.active_job_id.empty()) {
            to_cancel.push_back(alloc.slurm_id);
        }
    }

    for (const auto& id : to_cancel) {
        deallocate(id, cb);
    }

    if (to_cancel.empty() && cb) {
        cb("No idle allocations to cancel");
    }
}
