#include "tccp_service.hpp"
#include <core/time_utils.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>
#include <ctime>

TccpService::TccpService() {
    auto config_result = Config::load();
    if (config_result.is_ok()) {
        config_ = config_result.value;
    }
}

TccpService::~TccpService() {
    disconnect();
}

// ── Connection lifecycle ──────────────────────────────────────

Result<void> TccpService::connect(StatusCallback cb) {
    if (!config_.has_value()) {
        auto config_result = Config::load();
        if (config_result.is_err()) {
            return Result<void>::Err(config_result.error);
        }
        config_ = config_result.value;
    }

    cluster_ = std::make_unique<ConnectionFactory>(config_.value());
    auto result = cluster_->connect(cb);
    if (result.failed()) {
        cluster_.reset();
        return Result<void>::Err("Connection failed: " + result.stderr_data);
    }

    init_managers();
    return Result<void>::Ok();
}

void TccpService::disconnect() {
    clear_managers();
    if (cluster_) {
        cluster_->disconnect();
        cluster_.reset();
    }
}

bool TccpService::is_connected() const {
    return cluster_ && cluster_->is_connected();
}

bool TccpService::check_alive() {
    return cluster_ && cluster_->check_alive();
}

bool TccpService::check_slurm_health() {
    if (!cluster_ || !cluster_->is_connected()) return false;
    auto result = cluster_->login().run("sinfo -h 2>&1");
    return result.exit_code == 0;
}

// ── Job operations ────────────────────────────────────────────

Result<TrackedJob> TccpService::run_job(const std::string& job_name, const std::string& extra_args,
                                        StatusCallback cb) {
    if (!jobs_) return Result<TrackedJob>::Err("Not connected");
    return jobs_->run(job_name, extra_args, cb);
}

Result<void> TccpService::cancel_job(const std::string& job_name, StatusCallback cb) {
    if (!jobs_) return Result<void>::Err("Not connected");
    return jobs_->cancel_job(job_name, cb);
}

Result<void> TccpService::cancel_job_by_id(const std::string& job_id, StatusCallback cb) {
    if (!jobs_) return Result<void>::Err("Not connected");
    return jobs_->cancel_job_by_id(job_id, cb);
}

Result<void> TccpService::return_output(const std::string& job_id, StatusCallback cb) {
    if (!jobs_) return Result<void>::Err("Not connected");
    return jobs_->return_output(job_id, cb);
}

TrackedJob* TccpService::find_job_by_name(const std::string& job_name) {
    if (!jobs_) return nullptr;
    return jobs_->find_by_name(job_name);
}

std::vector<JobSummary> TccpService::list_jobs() {
    std::vector<JobSummary> summaries;
    if (!jobs_) return summaries;

    // Reconcile allocation state first for accuracy
    if (allocs_) {
        allocs_->reconcile(nullptr);
    }

    const auto& tracked = jobs_->tracked_jobs();
    for (const auto& tj : tracked) {
        JobSummary s;
        s.job_id = tj.job_id;
        s.job_name = tj.job_name;
        s.slurm_id = tj.slurm_id;
        s.compute_node = tj.compute_node;
        s.timestamp = format_timestamp(tj.submit_time);

        // Derive status, wait, and duration from raw TrackedJob fields
        if (!tj.init_error.empty()) {
            s.status = "ABORTED";
            s.wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);
            s.duration = "-";
        } else if (tj.canceled && !tj.init_complete) {
            s.status = "ABORTED";
            s.wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);
            s.duration = "-";
        } else if (!tj.init_complete) {
            s.status = "INITIALIZING";
            s.wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);
            s.duration = "-";
        } else if (tj.completed && tj.canceled) {
            s.status = "CANCELED";
            s.wait = format_duration(tj.submit_time, tj.start_time);
            s.duration = format_duration(tj.start_time, tj.end_time);
        } else if (tj.completed) {
            s.status = tj.exit_code == 0 ? "COMPLETED" : fmt::format("FAILED (exit {})", tj.exit_code);
            s.wait = format_duration(tj.submit_time, tj.start_time);
            s.duration = format_duration(tj.start_time, tj.end_time);
        } else if (!tj.compute_node.empty()) {
            s.status = "RUNNING";
            s.wait = format_duration(tj.submit_time, tj.start_time);
            s.duration = format_duration(tj.start_time);
        } else {
            s.status = "PENDING";
            s.wait = "-";
            s.duration = "-";
        }

        // Port forwarding info
        if (!tj.forwarded_ports.empty()) {
            std::string port_str;
            for (int p : tj.forwarded_ports) {
                if (!port_str.empty()) port_str += ",";
                port_str += std::to_string(p);
            }
            s.ports = port_str;
        }

        summaries.push_back(std::move(s));
    }

    return summaries;
}

PollResult TccpService::poll_jobs() {
    PollResult result;
    if (jobs_) {
        result = jobs_->poll();
    }
    // Release idle allocations that can no longer fit any configured job
    if (allocs_ && config_) {
        allocs_->reap_expired_idle(config_.value());
    }
    return result;
}

void TccpService::prune_old_jobs() {
    if (jobs_) {
        jobs_->prune_completed_jobs();
    }
}

// ── Allocation operations ─────────────────────────────────────

std::vector<AllocationSummary> TccpService::list_allocations() {
    std::vector<AllocationSummary> summaries;
    if (!allocs_) return summaries;

    const auto& allocs = allocs_->allocations();
    for (const auto& a : allocs) {
        AllocationSummary s;
        s.slurm_id = a.slurm_id;
        s.node = a.node.empty() ? "(waiting)" : a.node;
        s.partition = a.partition.empty() ? "batch" : a.partition;

        // GPU info
        if (!a.gpu_type.empty() || a.gpu_count > 0) {
            s.gpu = fmt::format("{}:{}", a.gpu_type.empty() ? "gpu" : a.gpu_type,
                                a.gpu_count > 0 ? a.gpu_count : 1);
        } else {
            s.gpu = "-";
        }

        // Status
        if (a.node.empty()) {
            s.status = "PENDING";
        } else if (a.active_job_id.empty()) {
            s.status = "IDLE";
        } else {
            s.status = "BUSY";
        }

        // Remaining time
        s.remaining = "-";
        if (!a.start_time.empty()) {
            auto start = parse_iso_time(a.start_time);
            if (start > 0) {
                auto now = std::time(nullptr);
                int elapsed_min = static_cast<int>(std::difftime(now, start) / 60.0);
                int rem = a.duration_minutes - elapsed_min;
                if (rem < 0) rem = 0;
                int h = rem / 60, m = rem % 60;
                s.remaining = fmt::format("{}h {:02d}m", h, m);
            }
        }

        summaries.push_back(std::move(s));
    }

    return summaries;
}

void TccpService::reconcile_allocations(StatusCallback cb) {
    if (allocs_) {
        allocs_->reconcile(cb);
    }
}

void TccpService::deallocate(const std::string& slurm_id, StatusCallback cb) {
    if (!allocs_) return;
    if (slurm_id.empty()) {
        allocs_->deallocate_all_idle(cb);
    } else {
        allocs_->deallocate(slurm_id, cb);
    }
}

// ── Shutdown ──────────────────────────────────────────────────

int TccpService::initializing_job_count() {
    if (!jobs_) return 0;
    int count = 0;
    for (const auto& tj : jobs_->tracked_jobs()) {
        if (!tj.init_complete && !tj.completed) {
            count++;
        }
    }
    return count;
}

void TccpService::cancel_initializing_jobs(StatusCallback cb) {
    if (!jobs_) return;

    // Collect IDs of initializing jobs (and their allocation IDs)
    std::vector<std::pair<std::string, std::string>> to_cancel; // {job_id, slurm_id}
    for (const auto& tj : jobs_->tracked_jobs()) {
        if (!tj.init_complete && !tj.completed) {
            to_cancel.push_back({tj.job_id, tj.slurm_id});
        }
    }

    for (const auto& [job_id, slurm_id] : to_cancel) {
        if (cb) cb(fmt::format("Canceling initializing job '{}'", job_id));
        jobs_->cancel_job_by_id(job_id, nullptr);

        // Deallocate the allocation this job was using
        if (!slurm_id.empty() && allocs_) {
            if (cb) cb(fmt::format("Releasing allocation {}", slurm_id));
            allocs_->deallocate(slurm_id, nullptr);
        }
    }
}

void TccpService::graceful_shutdown(StatusCallback cb) {
    cancel_initializing_jobs(cb);
    disconnect();
}

// ── Shell operations ──────────────────────────────────────────

SSHResult TccpService::exec_remote(const std::string& command) {
    if (!cluster_ || !cluster_->is_connected()) {
        return SSHResult{-1, "", "Not connected"};
    }
    return cluster_->login().run(command);
}

// ── State queries ─────────────────────────────────────────────

void TccpService::reload_config() {
    auto config_result = Config::load();
    if (config_result.is_ok()) {
        config_ = config_result.value;
    }
}

// ── Private ───────────────────────────────────────────────────

void TccpService::init_managers() {
    if (config_ && cluster_ && cluster_->is_connected()) {
        state_store_ = std::make_unique<StateStore>(config_.value().project().name);
        allocs_ = std::make_unique<AllocationManager>(
            config_.value(), cluster_->dtn(), cluster_->login(), *state_store_);
        allocs_->reconcile();
        sync_ = std::make_unique<SyncManager>(config_.value(), cluster_->dtn());
        cache_ = std::make_unique<CacheManager>(cluster_->dtn(), get_cluster_username());
        jobs_ = std::make_unique<JobManager>(
            config_.value(), *cluster_, *allocs_, *sync_, *cache_);
    }
}

void TccpService::clear_managers() {
    jobs_.reset();
    cache_.reset();
    sync_.reset();
    allocs_.reset();
    state_store_.reset();
}
