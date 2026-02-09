#pragma once

#include <string>
#include <vector>
#include <memory>
#include <core/config.hpp>
#include <ssh/connection.hpp>

struct TrackedJob {
    std::string job_id;       // YYYY-MM-DDTHH-MM-SS-mmm__<job-name>
    std::string slurm_id;     // SLURM numeric ID
    std::string job_name;     // e.g. "train"
    std::string compute_node; // filled once RUNNING
    bool completed = false;
    int exit_code = -1;      // -1 = unknown/still running
    std::string output_file; // local path to captured output
};

class JobManager {
public:
    JobManager(const Config& config, SSHConnection& dtn, SSHConnection& login);

    Result<TrackedJob> run(const std::string& job_name, StatusCallback cb = nullptr);
    void stage_to_node(const std::string& job_id,
                       const std::string& compute_node, StatusCallback cb);
    void collect_remote_logs(const std::string& job_id,
                             const std::string& slurm_id);
    SSHResult list(StatusCallback cb = nullptr);
    SSHResult cancel(const std::string& slurm_id, StatusCallback cb = nullptr);
    Result<void> return_output(const std::string& job_id, StatusCallback cb = nullptr);
    void poll(std::function<void(const TrackedJob&)> on_complete);
    const std::vector<TrackedJob>& tracked_jobs() const;
    TrackedJob* find_by_name(const std::string& job_name);

private:
    const Config& config_;
    SSHConnection& dtn_;
    SSHConnection& login_;
    std::string username_;
    std::vector<TrackedJob> tracked_;

    // Path helpers
    std::string persistent_base() const;
    std::string job_dir(const std::string& job_id) const;

    // Staging
    std::string generate_job_id(const std::string& job_name) const;
    void ensure_dirs(const std::string& job_id, StatusCallback cb);
    void ensure_environment(StatusCallback cb);
    void ensure_dtach(StatusCallback cb);
    std::string generate_slurm_script(const std::string& job_id,
                                       const std::string& job_name) const;
    std::string submit(const std::string& job_id, StatusCallback cb);

    // Polling helpers
    struct JobState {
        std::string state;
        std::string node;
    };
    JobState query_job_state(const std::string& slurm_id);
};
