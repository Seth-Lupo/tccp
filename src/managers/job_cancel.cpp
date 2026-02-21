#include "job_manager.hpp"
#include "job_log.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>

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
