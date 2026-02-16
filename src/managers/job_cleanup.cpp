#include "job_manager.hpp"
#include "job_log.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>
#include <sstream>
#include <set>
#include <map>
#include <filesystem>

// ── Cancel ─────────────────────────────────────────────────

Result<void> JobManager::do_cancel(TrackedJob* tj, const std::string& job_name) {
    // If job is still initializing, request cancellation
    if (!tj->init_complete) {
        // Mark for cancellation (init thread will check this)
        {
            std::lock_guard<std::mutex> lock(cancel_mutex_);
            cancel_requested_.insert(tj->job_id);
        }

        // Mark as canceled immediately
        tj->canceled = true;
        tj->completed = true;
        tj->exit_code = 130;
        tj->end_time = now_iso();
        append_job_log(tj->job_id, "Job canceled");
        persist_job_state(*tj);
        allocs_.persist();

        return Result<void>::Ok();
    }

    // Check REMOTE state (don't trust local completed flag)
    std::string dtach_socket = tj->scratch_path + "/tccp.sock";

    std::string check_cmd = fmt::format(
        "ssh {} {} 'test -e {} && echo RUNNING || echo DONE'",
        SSH_OPTS_FAST, tj->compute_node, dtach_socket);

    auto check_result = dtn_.run(check_cmd);
    tccp_log_ssh("cancel:check-remote", check_cmd, check_result);

    // If remote shows job is done, update local state and return error
    if (check_result.stdout_data.find("DONE") != std::string::npos) {
        tj->completed = true;
        persist_job_state(*tj);
        allocs_.persist();

        if (tj->canceled) {
            return Result<void>::Err("Job '" + job_name + "' already canceled");
        } else {
            return Result<void>::Err(fmt::format("Job '{}' already completed (exit {})", job_name, tj->exit_code));
        }
    }

    // Mark as canceled and finalize
    tj->canceled = true;
    tj->completed = true;
    tj->exit_code = 130;  // 128 + SIGINT
    tj->end_time = now_iso();
    append_job_log(tj->job_id, "Job canceled");

    cleanup_compute_node(*tj);
    persist_job_state(*tj);

    if (!tj->slurm_id.empty()) {
        allocs_.release_job(tj->slurm_id);
    } else {
        allocs_.persist();
    }

    return Result<void>::Ok();
}

Result<void> JobManager::cancel_job(const std::string& job_name, StatusCallback cb) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);

    // Find the most recent job by name (last match = newest)
    TrackedJob* tj = nullptr;
    for (auto& job : tracked_) {
        if (job.job_name == job_name) {
            tj = &job;
        }
    }

    if (!tj) {
        return Result<void>::Err("Job '" + job_name + "' not found");
    }

    if (cb) cb(fmt::format("Canceling job '{}'...", job_name));

    auto result = do_cancel(tj, job_name);

    if (result.is_ok() && cb) {
        if (tj->slurm_id.empty()) {
            cb(fmt::format("Job '{}' canceled during initialization", job_name));
        } else {
            cb(fmt::format("Job '{}' canceled (allocation {} kept alive)", job_name, tj->slurm_id));
        }
    }

    return result;
}

Result<void> JobManager::cancel_job_by_id(const std::string& job_id, StatusCallback cb) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);

    // Find job by ID
    TrackedJob* tj = nullptr;
    for (auto& job : tracked_) {
        if (job.job_id == job_id) {
            tj = &job;
            break;
        }
    }

    if (!tj) {
        return Result<void>::Err("Job ID '" + job_id + "' not found");
    }

    if (cb) cb(fmt::format("Canceling job '{}'...", tj->job_name));

    auto result = do_cancel(tj, tj->job_name);

    if (result.is_ok() && cb) {
        if (tj->slurm_id.empty()) {
            cb(fmt::format("Job '{}' canceled during initialization", tj->job_name));
        } else {
            cb(fmt::format("Job '{}' canceled", tj->job_name));
        }
    }

    return result;
}

// ── Poll ───────────────────────────────────────────────────

JobManager::SlurmJobState JobManager::query_alloc_state(const std::string& slurm_id) {
    std::string cmd = fmt::format("squeue -j {} -o \"%T %N\" -h", slurm_id);
    auto result = login_.run(cmd);

    SlurmJobState state;
    if (result.success()) {
        std::istringstream iss(result.stdout_data);
        iss >> state.state >> state.node;
    }
    return state;
}

void JobManager::poll(std::function<void(const TrackedJob&)> on_complete) {
    // Phase 1: Detect completed jobs while holding the lock.
    std::vector<TrackedJob> newly_completed;

    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& tj : tracked_) {
            if (tj.completed) continue;
            if (!tj.init_complete) continue;

            if (!tj.compute_node.empty()) {
                std::string sock = tj.scratch_path + "/tccp.sock";
                auto check = dtn_.run(fmt::format("ssh {} {} 'test -e {} && echo RUNNING || echo DONE'",
                                                  SSH_OPTS_FAST, tj.compute_node, sock));

                if (check.stdout_data.find("DONE") != std::string::npos) {
                    tj.completed = true;
                    tj.exit_code = 0;
                    tj.end_time = now_iso();
                    newly_completed.push_back(tj);
                } else if (check.stdout_data.find("RUNNING") == std::string::npos) {
                    auto alloc_state = query_alloc_state(tj.slurm_id);
                    if (alloc_state.state.empty() || alloc_state.state == "COMPLETED" ||
                        alloc_state.state == "FAILED" || alloc_state.state == "CANCELLED") {
                        tj.completed = true;
                        tj.exit_code = -1;
                        tj.end_time = now_iso();
                        newly_completed.push_back(tj);
                        tccp_log(fmt::format("poll: allocation {} gone ({}), marking job {} complete",
                                             tj.slurm_id, alloc_state.state, tj.job_id));
                    }
                }
            } else {
                auto alloc_state = query_alloc_state(tj.slurm_id);
                if (alloc_state.state == "RUNNING" && !alloc_state.node.empty()) {
                    tj.compute_node = alloc_state.node;
                } else if (alloc_state.state.empty() || alloc_state.state == "COMPLETED" ||
                           alloc_state.state == "FAILED" || alloc_state.state == "CANCELLED") {
                    tj.completed = true;
                    tj.exit_code = -1;
                    tj.end_time = now_iso();
                    newly_completed.push_back(tj);
                }
            }
        }
    }

    // Phase 2: Process completions without the lock held.
    for (auto& tj : newly_completed) {
        if (tj.exit_code == 0) {
            append_job_log(tj.job_id, "Job completed (exit 0)");
        } else {
            append_job_log(tj.job_id, fmt::format("Job completed (exit {})", tj.exit_code));
        }
        allocs_.release_job(tj.slurm_id);
        cleanup_compute_node(tj);
        persist_job_state(tj);
        try_return_output(tj);
    }

    // Prune once after all completions
    if (!newly_completed.empty()) {
        prune_completed_jobs();
    }

    // Phase 3: User callbacks
    for (const auto& tj : newly_completed) {
        if (on_complete) on_complete(tj);
    }
}

// ── Cleanup helpers ────────────────────────────────────────

void JobManager::kill_job_processes(const TrackedJob& tj) {
    if (tj.compute_node.empty()) return;

    // Kill everything associated with this job on the compute node.
    // The process tree (dtach → bash → script → singularity → python) can have
    // orphaned children that survive if only the dtach PID is killed.  We use
    // multiple strategies to be thorough:
    //   1. fuser on dtach socket → walk /proc descendants → SIGKILL tree
    //   2. pkill -f matching the job_id (catches orphaned singularity/python)
    //   3. Remove the dtach socket
    std::string sock = tj.scratch_path + "/tccp.sock";
    std::string cmd = fmt::format(
        "ssh {} {} '"
        // Strategy 1: fuser + process tree walk
        "DPID=$(fuser {} 2>/dev/null | tr -d \" \"); "
        "if [ -n \"$DPID\" ]; then "
        "  PIDS=$DPID; "
        "  TODO=$DPID; "
        "  while [ -n \"$TODO\" ]; do "
        "    NEXT=\"\"; "
        "    for P in $TODO; do "
        "      for C in $(ps -o pid= --ppid $P 2>/dev/null); do "
        "        PIDS=\"$PIDS $C\"; NEXT=\"$NEXT $C\"; "
        "      done; "
        "    done; "
        "    TODO=$NEXT; "
        "  done; "
        "  kill -9 $PIDS 2>/dev/null; "
        "fi; "
        // Strategy 2: pkill anything referencing this job_id
        "pkill -9 -f \"{}\" 2>/dev/null; "
        // Clean up socket
        "rm -f {}'",
        SSH_OPTS_FAST, tj.compute_node, sock, tj.job_id, sock);
    auto result = dtn_.run(cmd);
    tccp_log_ssh("cleanup:kill", cmd, result);
}

void JobManager::cleanup_compute_node(const TrackedJob& tj) {
    if (!tj.tunnel_pids.empty()) {
        PortForwarder::stop(tj.tunnel_pids);
    }

    if (tj.compute_node.empty()) return;

    // Kill all job processes and remove the dtach socket.
    // Keep the scratch directory alive so the next job on the same node can
    // reuse the synced files (cp + incremental diff) instead of a full
    // re-upload.  Scratch is cleaned up later by sync_to_scratch.
    kill_job_processes(tj);
}

void JobManager::persist_job_state(const TrackedJob& tj) {
    auto& jobs = allocs_.state().jobs;
    for (auto& js : jobs) {
        if (js.job_id == tj.job_id) {
            js.completed = tj.completed;
            js.canceled = tj.canceled;
            js.exit_code = tj.exit_code;
            js.output_file = tj.output_file;
            js.end_time = tj.end_time;
            js.forwarded_ports = tj.forwarded_ports;
            break;
        }
    }
}

void JobManager::on_job_complete(TrackedJob& tj) {
    if (tj.exit_code == 0) {
        append_job_log(tj.job_id, "Job completed (exit 0)");
    } else {
        append_job_log(tj.job_id, fmt::format("Job completed (exit {})", tj.exit_code));
    }
    allocs_.release_job(tj.slurm_id);
    if (tj.end_time.empty()) tj.end_time = now_iso();
    cleanup_compute_node(tj);
    persist_job_state(tj);
    try_return_output(tj);
    prune_completed_jobs();
}

void JobManager::prune_completed_jobs() {
    auto& state_jobs = allocs_.state().jobs;

    // Group completed/canceled/failed jobs by name, keep only latest
    std::map<std::string, size_t> latest_by_name;
    std::vector<bool> keep(state_jobs.size(), false);

    for (size_t i = 0; i < state_jobs.size(); i++) {
        auto& js = state_jobs[i];
        bool is_terminal = js.completed || js.canceled || !js.init_error.empty();
        if (!is_terminal) {
            keep[i] = true;
            continue;
        }
        auto it = latest_by_name.find(js.job_name);
        if (it == latest_by_name.end()) {
            latest_by_name[js.job_name] = i;
        } else {
            if (js.submit_time > state_jobs[it->second].submit_time) {
                latest_by_name[js.job_name] = i;
            }
        }
    }
    for (auto& [name, idx] : latest_by_name) keep[idx] = true;

    // Compact state jobs
    std::vector<JobState> kept_jobs;
    std::set<std::string> kept_ids;
    for (size_t i = 0; i < state_jobs.size(); i++) {
        if (keep[i]) {
            kept_jobs.push_back(state_jobs[i]);
            kept_ids.insert(state_jobs[i].job_id);
        }
    }

    if (kept_jobs.size() < state_jobs.size()) {
        // Delete log files for pruned jobs
        for (size_t i = 0; i < state_jobs.size(); i++) {
            if (!keep[i]) {
                std::filesystem::remove(job_log_path(state_jobs[i].job_id));
            }
        }

        state_jobs = kept_jobs;

        {
            std::lock_guard<std::mutex> lock(tracked_mutex_);
            std::vector<TrackedJob> kept_tracked;
            for (auto& tj : tracked_) {
                if (kept_ids.count(tj.job_id)) {
                    kept_tracked.push_back(tj);
                }
            }
            tracked_ = kept_tracked;
        }

        allocs_.persist();
    }
}
