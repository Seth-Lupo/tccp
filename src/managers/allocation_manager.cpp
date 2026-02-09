#include "allocation_manager.hpp"
#include <core/config.hpp>
#include <core/credentials.hpp>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <ctime>

static std::string get_cluster_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "unknown";
}

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

// Parse ISO timestamp to time_t
static std::time_t parse_iso(const std::string& iso) {
    struct tm tm_buf = {};
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
               &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec) == 6) {
        tm_buf.tm_year -= 1900;
        tm_buf.tm_mon -= 1;
        return mktime(&tm_buf);
    }
    return 0;
}

AllocationManager::AllocationManager(const Config& config, SSHConnection& dtn,
                                     SSHConnection& login, StateStore& store)
    : config_(config), dtn_(dtn), login_(login), store_(store),
      username_(get_cluster_username()) {
}

std::string AllocationManager::persistent_base() const {
    return fmt::format("/cluster/home/{}/tccp/projects/{}",
                       username_, config_.project().name);
}

std::string AllocationManager::container_cache() const {
    return fmt::format("/cluster/home/{}/tccp/container-cache", username_);
}

void AllocationManager::persist() {
    store_.save(state_);
}

int AllocationManager::parse_time_minutes(const std::string& time_str) {
    if (time_str.empty()) return 240; // default 4 hours

    int h = 0, m = 0, s = 0;
    if (sscanf(time_str.c_str(), "%d:%d:%d", &h, &m, &s) >= 2) {
        return h * 60 + m + (s > 0 ? 1 : 0);
    }
    return 240;
}

void AllocationManager::reconcile(StatusCallback cb) {
    state_ = store_.load();

    if (state_.allocations.empty()) {
        if (cb) cb("No previous allocations found");
        return;
    }

    if (cb) cb(fmt::format("Reconciling {} allocation(s) with SLURM...",
                           state_.allocations.size()));

    std::vector<AllocationState> live;
    for (auto& alloc : state_.allocations) {
        std::string cmd = fmt::format("squeue -j {} -h -o \"%T %N\"", alloc.slurm_id);
        auto result = login_.run(cmd);

        std::string state_str, node;
        std::istringstream iss(result.stdout_data);
        iss >> state_str >> node;

        if (state_str == "RUNNING") {
            if (!node.empty()) alloc.node = node;
            live.push_back(alloc);
            if (cb) cb(fmt::format("  Allocation {} RUNNING on {}", alloc.slurm_id, alloc.node));
        } else if (state_str == "PENDING") {
            live.push_back(alloc);
            if (cb) cb(fmt::format("  Allocation {} PENDING", alloc.slurm_id));
        } else {
            if (cb) cb(fmt::format("  Allocation {} gone ({})", alloc.slurm_id,
                                   state_str.empty() ? "not found" : state_str));
        }
    }

    state_.allocations = live;
    persist();
}

AllocationState* AllocationManager::find_free(int required_minutes) {
    for (auto& alloc : state_.allocations) {
        if (alloc.active_job_id.empty() && !alloc.node.empty()) {
            int rem = remaining_minutes(alloc);
            if (rem >= required_minutes) {
                return &alloc;
            }
        }
    }
    return nullptr;
}

AllocationState* AllocationManager::find_pending() {
    for (auto& alloc : state_.allocations) {
        if (alloc.active_job_id.empty() && alloc.node.empty()) {
            return &alloc;
        }
    }
    return nullptr;
}

Result<AllocationState> AllocationManager::wait_for_allocation(
        const std::string& slurm_id, StatusCallback cb) {
    // Poll until RUNNING (up to 10 minutes)
    for (int i = 0; i < 120; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        std::string cmd = fmt::format("squeue -j {} -h -o \"%T %N\"", slurm_id);
        auto qr = login_.run(cmd);

        std::string state_str, node;
        std::istringstream iss(qr.stdout_data);
        iss >> state_str >> node;

        if (state_str == "RUNNING" && !node.empty()) {
            // Update the persisted allocation with node info
            for (auto& alloc : state_.allocations) {
                if (alloc.slurm_id == slurm_id) {
                    alloc.node = node;
                    alloc.start_time = now_iso();
                    persist();
                    if (cb) cb(fmt::format("Allocation {} running on {}", slurm_id, node));
                    return Result<AllocationState>::Ok(alloc);
                }
            }
        }

        if (state_str == "COMPLETED" || state_str == "FAILED" ||
            state_str == "CANCELLED") {
            // Truly dead — remove from state
            auto& allocs = state_.allocations;
            allocs.erase(
                std::remove_if(allocs.begin(), allocs.end(),
                    [&](const AllocationState& a) { return a.slurm_id == slurm_id; }),
                allocs.end());
            persist();
            return Result<AllocationState>::Err(
                fmt::format("Allocation {} died: {}", slurm_id, state_str));
        }

        // Empty state_str likely means SSH connection issue — don't kill
        // the allocation, just report the error so caller can retry later
        if (state_str.empty()) {
            return Result<AllocationState>::Err(
                fmt::format("Lost connection while waiting for allocation {}", slurm_id));
        }

        // Still PENDING — keep waiting
        if (i % 6 == 0 && cb) {
            cb(fmt::format("Still waiting for allocation {}... ({}s)", slurm_id, (i + 1) * 5));
        }
    }

    // Timed out — cancel and remove
    login_.run("scancel " + slurm_id);
    auto& allocs = state_.allocations;
    allocs.erase(
        std::remove_if(allocs.begin(), allocs.end(),
            [&](const AllocationState& a) { return a.slurm_id == slurm_id; }),
        allocs.end());
    persist();
    return Result<AllocationState>::Err("Timed out waiting for allocation");
}

int AllocationManager::remaining_minutes(const AllocationState& alloc) const {
    if (alloc.start_time.empty()) return 0;

    auto start = parse_iso(alloc.start_time);
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
    }

    // Default allocation time if not set
    if (profile.time.empty()) {
        profile.time = "4:00:00";
    }

    return profile;
}

std::string AllocationManager::generate_alloc_script(const SlurmDefaults& profile) const {
    std::string base = persistent_base();

    std::string s;
    s += "#!/bin/bash\n";
    s += fmt::format("#SBATCH --job-name=tccp-{}\n", config_.project().name);
    s += "#SBATCH --partition=batch\n";
    s += fmt::format("#SBATCH --time={}\n", profile.time);
    s += "#SBATCH --nodes=1\n";
    s += "#SBATCH --cpus-per-task=1\n";
    s += "#SBATCH --mem=1G\n";
    s += fmt::format("#SBATCH --output={}/alloc-%j.out\n", base);
    s += fmt::format("#SBATCH --error={}/alloc-%j.err\n", base);
    s += "\nsleep infinity\n";

    return s;
}

Result<AllocationState> AllocationManager::allocate(const SlurmDefaults& profile,
                                                     StatusCallback cb) {
    if (cb) cb("Requesting compute allocation...");

    // Ensure base directories exist
    std::string base = persistent_base();
    std::string cc = container_cache();
    dtn_.run(fmt::format("mkdir -p {} {}/env {}/images {}/cache {}/tmp",
                         base, base, cc, cc, cc));

    // Submit allocation via login node using heredoc pipe to sbatch
    std::string script_content = generate_alloc_script(profile);
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

    // Create allocation state and persist IMMEDIATELY so it survives
    // connection drops. We'll update node/start_time once RUNNING.
    AllocationState alloc;
    alloc.slurm_id = slurm_id;
    alloc.duration_minutes = parse_time_minutes(profile.time);
    state_.allocations.push_back(alloc);
    persist();

    return wait_for_allocation(slurm_id, cb);
}

void AllocationManager::assign_job(const std::string& slurm_id, const std::string& job_id) {
    for (auto& alloc : state_.allocations) {
        if (alloc.slurm_id == slurm_id) {
            alloc.active_job_id = job_id;
            persist();
            return;
        }
    }
}

void AllocationManager::release_job(const std::string& slurm_id) {
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

    auto& allocs = state_.allocations;
    allocs.erase(
        std::remove_if(allocs.begin(), allocs.end(),
                       [&](const AllocationState& a) { return a.slurm_id == slurm_id; }),
        allocs.end());
    persist();
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
