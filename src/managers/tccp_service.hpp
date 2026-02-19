#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <core/config.hpp>
#include <ssh/connection_factory.hpp>
#include "state_store.hpp"
#include "allocation_manager.hpp"
#include "sync_manager.hpp"
#include "cache_manager.hpp"
#include "job_manager.hpp"

// Pure data structs for UI consumption — no manager dependencies needed.

struct JobSummary {
    std::string job_id;
    std::string job_name;
    std::string slurm_id;
    std::string compute_node;
    std::string status;       // "INITIALIZING", "RUNNING", "COMPLETED", "FAILED (exit N)", etc.
    std::string timestamp;    // formatted submit time (HH:MM)
    std::string wait;         // formatted wait duration
    std::string duration;     // formatted run duration
    std::string ports;         // forwarded ports, e.g. "6006,8888"
};

struct AllocationSummary {
    std::string slurm_id;
    std::string node;
    std::string partition;
    std::string gpu;          // e.g. "a100:2" or "-"
    std::string status;       // "PENDING", "IDLE", "BUSY"
    std::string remaining;    // e.g. "2h 35m" or "-"
};

// Headless service facade — owns all managers, can be used by any frontend.
class TccpService {
public:
    TccpService();
    ~TccpService();

    // ── Connection lifecycle ──────────────────────────────────

    // Connect to the cluster. Loads config, creates ConnectionFactory, inits managers.
    // Returns error string on failure, empty on success.
    Result<void> connect(StatusCallback cb = nullptr);

    // Disconnect and tear down all managers.
    void disconnect();

    // Check if connected and alive.
    bool is_connected() const;
    bool check_alive();

    // Check SLURM health (sinfo responds).
    bool check_slurm_health();

    // ── Job operations ────────────────────────────────────────

    // Submit a job by name, with optional runtime args appended to config args.
    Result<TrackedJob> run_job(const std::string& job_name, const std::string& extra_args = "",
                               StatusCallback cb = nullptr);

    // Cancel a job by name.
    Result<void> cancel_job(const std::string& job_name, StatusCallback cb = nullptr);

    // Cancel a job by internal ID.
    Result<void> cancel_job_by_id(const std::string& job_id, StatusCallback cb = nullptr);

    // Download job output.
    Result<void> return_output(const std::string& job_id, StatusCallback cb = nullptr);

    // Find a tracked job by name.
    TrackedJob* find_job_by_name(const std::string& job_name);

    // Get a snapshot of all tracked jobs as UI-ready summaries.
    std::vector<JobSummary> list_jobs();

    // Poll for completed jobs (calls on_complete for each newly finished job).
    void poll_jobs(std::function<void(const TrackedJob&)> on_complete);

    // Prune old terminated jobs, keeping only the latest per job name.
    void prune_old_jobs();

    // ── Allocation operations ─────────────────────────────────

    // Get a snapshot of all allocations as UI-ready summaries.
    std::vector<AllocationSummary> list_allocations();

    // Reconcile allocation state with SLURM.
    void reconcile_allocations(StatusCallback cb = nullptr);

    // Deallocate by SLURM ID, or all idle if slurm_id is empty.
    void deallocate(const std::string& slurm_id, StatusCallback cb = nullptr);

    // ── Shutdown ──────────────────────────────────────────────

    // Returns the count of jobs currently initializing.
    int initializing_job_count();

    // Cancel all initializing jobs and deallocate their allocations.
    void cancel_initializing_jobs(StatusCallback cb = nullptr);

    // Graceful shutdown: cancel init jobs, release idle allocs, disconnect.
    void graceful_shutdown(StatusCallback cb = nullptr);

    // ── Shell operations ──────────────────────────────────────

    // Execute a command on the login node.
    SSHResult exec_remote(const std::string& command);

    // ── State queries ─────────────────────────────────────────

    bool has_config() const { return config_.has_value(); }
    const Config& config() const { return config_.value(); }
    void reload_config();

    // ── Direct accessors for CLI-specific needs (e.g. JobView) ──

    ConnectionFactory* cluster() { return cluster_.get(); }
    JobManager* job_manager() { return jobs_.get(); }
    AllocationManager* alloc_manager() { return allocs_.get(); }
    StateStore* state_store() { return state_store_.get(); }

private:
    std::optional<Config> config_;
    std::unique_ptr<ConnectionFactory> cluster_;
    std::unique_ptr<StateStore> state_store_;
    std::unique_ptr<AllocationManager> allocs_;
    std::unique_ptr<SyncManager> sync_;
    std::unique_ptr<CacheManager> cache_;
    std::unique_ptr<JobManager> jobs_;

    void init_managers();
    void clear_managers();
};
