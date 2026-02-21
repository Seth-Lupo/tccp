#include "job_manager.hpp"
#include "job_log.hpp"
#include <core/utils.hpp>
#include <fmt/format.h>

// ── Poll Watcher Integration ────────────────────────────────

void JobManager::start_poll_watcher() {
    if (poll_watcher_) return;

    auto* mux = factory_.multiplexer();
    if (!mux) {
        tccp_log("poll_watcher: no multiplexer available, skipping");
        return;
    }

    poll_watcher_ = std::make_unique<JobPollWatcher>(
        *mux,
        [this](const std::string& job_id) {
            on_watcher_completion(job_id);
        });

    if (!poll_watcher_->start()) {
        poll_watcher_.reset();
        return;
    }

    refresh_watcher_targets();
}

void JobManager::stop_poll_watcher() {
    if (poll_watcher_) {
        poll_watcher_->stop();
        poll_watcher_.reset();
    }
}

void JobManager::on_watcher_completion(const std::string& job_id) {
    TrackedJob completed_copy;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& tj : tracked_) {
            if (tj.job_id == job_id) {
                if (tj.completed) return;  // Already handled by fallback poll
                tj.completed = true;
                tj.exit_code = 0;
                tj.end_time = now_iso();
                completed_copy = tj;
                found = true;
                break;
            }
        }
    }

    if (!found) return;

    // Post-completion pipeline (same as poll() Phase 2)
    if (completed_copy.exit_code == 0) {
        append_job_log(completed_copy.job_id, "Job completed (exit 0)");
    } else {
        append_job_log(completed_copy.job_id,
                       fmt::format("Job completed (exit {})", completed_copy.exit_code));
    }

    allocs_.release_job(completed_copy.slurm_id);
    cleanup_compute_node(completed_copy);
    persist_job_state(completed_copy);
    try_return_output(completed_copy);
    prune_completed_jobs();

    // Update watcher targets (this job is done)
    refresh_watcher_targets();
}

void JobManager::refresh_watcher_targets() {
    if (!poll_watcher_) return;

    std::vector<JobPollWatcher::WatchTarget> targets;
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (const auto& tj : tracked_) {
            if (tj.completed || !tj.init_complete) continue;
            if (tj.compute_node.empty() || tj.scratch_path.empty()) continue;
            targets.push_back({tj.job_id, tj.compute_node, tj.scratch_path});
        }
    }

    poll_watcher_->update_targets(std::move(targets));
}
