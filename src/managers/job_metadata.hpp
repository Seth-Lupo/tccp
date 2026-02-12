#pragma once

#include <string>
#include <filesystem>
#include <ssh/connection.hpp>
#include <core/types.hpp>

namespace fs = std::filesystem;

class JobMetadata {
public:
    // Fields
    std::string job_id;
    std::string job_name;
    std::string submit_time;
    std::string start_time;
    std::string end_time;
    std::string slurm_id;
    std::string compute_node;
    std::string scratch_path;
    int exit_code = -1;
    bool completed = false;
    bool canceled = false;
    bool init_complete = false;
    std::string init_error;

    // Static factory methods
    static Result<JobMetadata> load_local(const std::string& project_name, const std::string& job_id);
    static Result<JobMetadata> load_remote(SSHConnection& dtn, const std::string& project_name,
                                           const std::string& username, const std::string& job_id);

    // Instance methods
    void save_local(const std::string& project_name) const;
    void save_remote(SSHConnection& dtn, const std::string& project_name, const std::string& username) const;

    // Reconciliation: merge remote truth with local-only fields
    static JobMetadata reconcile(const JobMetadata& local, const Result<JobMetadata>& remote_result);

    // Utility: get local metadata directory path
    static fs::path local_jobs_dir(const std::string& project_name);
    static fs::path local_metadata_path(const std::string& project_name, const std::string& job_id);

    // Utility: get remote metadata path
    static std::string remote_metadata_path(const std::string& project_name,
                                            const std::string& username,
                                            const std::string& job_id);
};
