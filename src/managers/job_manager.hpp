#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <atomic>
#include <core/config.hpp>
#include <ssh/connection.hpp>
#include "allocation_manager.hpp"
#include "sync_manager.hpp"
#include "cache_manager.hpp"
#include "port_forwarder.hpp"

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
    std::string extra_args;   // runtime args from CLI (appended to config args)

    // Init tracking (for background initialization)
    bool init_complete = false;      // true when job is launched on compute node
    std::string init_error;           // error message if init failed

    // Output tracking
    bool output_returned = false;    // true if output was downloaded and remote cleaned

    // Port forwarding
    std::vector<pid_t> tunnel_pids;
    std::vector<int> forwarded_ports;

    // Timing
    std::string submit_time;          // ISO timestamp when job was first submitted
    std::string start_time;           // ISO timestamp when job started running (init_complete)
    std::string end_time;             // ISO timestamp when job completed
};

class JobManager {
public:
    JobManager(const Config& config, SSHConnection& dtn, SSHConnection& login,
               AllocationManager& allocs, SyncManager& sync, CacheManager& cache);
    ~JobManager();

    // Clear in-memory tracked jobs (for state reset without destroying the manager)
    void clear_tracked();

    // Job ID utilities: "2026-02-15T14-44-10-637__main" → name / timestamp
    static std::string job_name_from_id(const std::string& job_id);
    static std::string timestamp_from_id(const std::string& job_id);

    // Returns true for job names that are always available without being in tccp.yaml
    // (e.g. "bash"). Add new implicit job types here.
    static bool is_implicit_job(const std::string& job_name);

    Result<TrackedJob> run(const std::string& job_name, const std::string& extra_args = "",
                           StatusCallback cb = nullptr);
    SSHResult list(StatusCallback cb = nullptr);
    Result<void> cancel_job(const std::string& job_name, StatusCallback cb = nullptr);
    Result<void> cancel_job_by_id(const std::string& job_id, StatusCallback cb = nullptr);
    Result<void> return_output(const std::string& job_id, StatusCallback cb = nullptr);
    void poll(std::function<void(const TrackedJob&)> on_complete);
    const std::vector<TrackedJob>& tracked_jobs() const;
    TrackedJob* find_by_name(const std::string& job_name);

    // Build a shell command that enters the job's environment (singularity + binds + env vars).
    // If inner_cmd is empty, launches an interactive bash shell.
    std::string shell_command(const TrackedJob& tj, const std::string& inner_cmd = "") const;

    // Release allocation when job completes
    void on_job_complete(TrackedJob& tj);

    // Cleanup: keep only latest completed/canceled/failed job per name
    void prune_completed_jobs();

private:
    const Config& config_;
    SSHConnection& dtn_;
    SSHConnection& login_;
    AllocationManager& allocs_;
    SyncManager& sync_;
    CacheManager& cache_;
    std::string username_;
    std::vector<TrackedJob> tracked_;
    std::mutex tracked_mutex_;
    std::vector<std::thread> init_threads_;
    std::set<std::string> cancel_requested_;  // job_ids marked for cancellation during init
    std::mutex cancel_mutex_;
    std::atomic<bool> shutdown_{false};        // signals init threads to exit
    bool environment_checked_ = false;  // Cache: only check environment once per session
    PortForwarder port_fwd_;

    // Path helpers (new directory structure)
    std::string persistent_base() const;
    std::string job_output_dir(const std::string& job_id) const;
    std::string env_dir() const;
    std::string container_cache() const;
    std::string scratch_dir(const std::string& job_id) const;

    std::string generate_job_id(const std::string& job_name) const;
    void ensure_environment(const std::string& compute_node, StatusCallback cb);
    void ensure_dtach(StatusCallback cb);
    void ensure_dirs(const std::string& job_id, StatusCallback cb);

    // Resolved environment paths used for run scripts and shell commands.
    // Computed once via resolve_env_paths(), shared across launch/shell methods.
    struct JobEnvPaths {
        std::string image;          // full path to .sif container
        std::string venv;           // venv directory
        std::string binds;          // singularity --bind flags (including --nv)
        std::string runtime_cache;  // temp dir for HOME/XDG/TMPDIR
        std::string req_filter;     // bash snippet to filter requirements.txt
        std::string log;            // log file path
        bool has_python;            // environment has python
    };
    JobEnvPaths resolve_env_paths(const std::string& job_id,
                                  const std::string& scratch) const;

    // Common environment preamble for run scripts (module load, cd, env vars, pip install).
    std::string build_run_preamble(const JobEnvPaths& paths,
                                   const std::string& scratch) const;

    // Job-type-specific payload: the command(s) appended after the preamble.
    // Override this pattern for new implicit job types.
    std::string build_job_payload(const std::string& job_name,
                                  const JobEnvPaths& paths,
                                  const std::string& extra_args = "") const;

    // Launch the job on the compute node via SSH + dtach
    Result<void> launch_on_node(const std::string& job_id,
                                 const std::string& job_name,
                                 const std::string& compute_node,
                                 const std::string& scratch,
                                 const std::string& extra_args,
                                 StatusCallback cb);

    // Background init thread (does allocation, sync, launch)
    void background_init_thread(std::string job_id, std::string job_name, std::string extra_args);

    // Polling helpers
    struct SlurmJobState {
        std::string state;
        std::string node;
    };
    SlurmJobState query_alloc_state(const std::string& slurm_id);

    // Shared cancel logic (used by both cancel_job and cancel_job_by_id)
    Result<void> do_cancel(TrackedJob* tj, const std::string& job_name);

    // Kill all processes associated with a job on its compute node
    void kill_job_processes(const TrackedJob& tj);

    // Shared cleanup: kill job processes + remove socket from compute node
    void cleanup_compute_node(const TrackedJob& tj);

    // Shared: persist tracked job fields to state store
    void persist_job_state(const TrackedJob& tj);

    // Auto-return output and clean remote
    void try_return_output(TrackedJob& tj);

    // Mark a job's output as returned in tracked + persisted state
    void mark_output_returned(const std::string& job_id);

    // Download output files from remote. Returns file count, -1 on error.
    int download_remote_output(const std::string& job_id,
                               const std::string& remote_output);
};
