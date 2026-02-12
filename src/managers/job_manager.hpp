#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <core/config.hpp>
#include <ssh/connection.hpp>
#include "allocation_manager.hpp"
#include "sync_manager.hpp"

struct TrackedJob {
    std::string job_id;       // YYYY-MM-DDTHH-MM-SS-mmm__<job-name>
    std::string slurm_id;     // allocation SLURM ID
    std::string job_name;     // e.g. "train"
    std::string compute_node; // filled once allocation is RUNNING
    bool completed = false;
    bool canceled = false;    // true if manually killed via 'cancel' command
    int exit_code = -1;       // -1 = unknown/still running
    std::string output_file;  // local path to captured output
    std::string scratch_path; // /tmp/{user}/{project}/{job_id}

    // Init tracking (for background initialization)
    bool init_complete = false;      // true when job is launched on compute node
    std::string init_error;           // error message if init failed

    // Timing
    std::string submit_time;          // ISO timestamp when job was first submitted
    std::string start_time;           // ISO timestamp when job started running (init_complete)
    std::string end_time;             // ISO timestamp when job completed
};

class JobManager {
public:
    JobManager(const Config& config, SSHConnection& dtn, SSHConnection& login,
               AllocationManager& allocs, SyncManager& sync);

    Result<TrackedJob> run(const std::string& job_name, StatusCallback cb = nullptr);
    SSHResult list(StatusCallback cb = nullptr);
    Result<void> cancel_job(const std::string& job_name, StatusCallback cb = nullptr);
    Result<void> cancel_job_by_id(const std::string& job_id, StatusCallback cb = nullptr);
    Result<void> return_output(const std::string& job_id, StatusCallback cb = nullptr);
    void poll(std::function<void(const TrackedJob&)> on_complete);
    const std::vector<TrackedJob>& tracked_jobs() const;
    TrackedJob* find_by_name(const std::string& job_name);

    // Release allocation when job completes
    void on_job_complete(TrackedJob& tj);

private:
    const Config& config_;
    SSHConnection& dtn_;
    SSHConnection& login_;
    AllocationManager& allocs_;
    SyncManager& sync_;
    std::string username_;
    std::vector<TrackedJob> tracked_;
    std::mutex tracked_mutex_;
    std::vector<std::thread> init_threads_;
    std::set<std::string> cancel_requested_;  // job_ids marked for cancellation during init
    std::mutex cancel_mutex_;
    bool environment_checked_ = false;  // Cache: only check environment once per session

    // Path helpers (new directory structure)
    std::string persistent_base() const;
    std::string job_output_dir(const std::string& job_id) const;
    std::string env_dir() const;
    std::string container_cache() const;
    std::string scratch_dir(const std::string& job_id) const;

    std::string generate_job_id(const std::string& job_name) const;
    void ensure_environment(StatusCallback cb);
    void ensure_dtach(StatusCallback cb);
    void ensure_dirs(const std::string& job_id, StatusCallback cb);

    // Launch the job on the compute node via SSH + dtach
    Result<void> launch_on_node(const std::string& job_id,
                                 const std::string& job_name,
                                 const std::string& compute_node,
                                 const std::string& scratch,
                                 StatusCallback cb);

    // Background init thread (does allocation, sync, launch)
    void background_init_thread(std::string job_id, std::string job_name);

    // Polling helpers
    struct SlurmJobState {
        std::string state;
        std::string node;
    };
    SlurmJobState query_alloc_state(const std::string& slurm_id);

    // Shared cancel logic (used by both cancel_job and cancel_job_by_id)
    Result<void> do_cancel(TrackedJob* tj, const std::string& job_name);
};
