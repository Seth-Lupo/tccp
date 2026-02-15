#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <core/config.hpp>
#include <core/resource_spec.hpp>
#include <ssh/connection.hpp>
#include "state_store.hpp"

class AllocationManager {
public:
    AllocationManager(const Config& config, SSHConnection& dtn,
                      SSHConnection& login, StateStore& store);

    // On startup: load state, query SLURM, prune dead allocations
    void reconcile(StatusCallback cb = nullptr);

    // Atomically find a free RUNNING allocation and assign a job to it.
    // Returns nullptr if no compatible allocation is available.
    // Thread-safe: prevents two init threads from claiming the same allocation.
    AllocationState* claim_free(int required_minutes, const SlurmDefaults& required_resources,
                                const std::string& job_id, StatusCallback cb = nullptr);

    // Find allocation by SLURM job ID (direct lookup, no resource matching)
    AllocationState* find_by_id(const std::string& slurm_id);

    // Find a PENDING allocation with compatible resources (submitted but no node yet)
    AllocationState* find_pending(const SlurmDefaults& required_resources);

    // Wait for a specific allocation to reach RUNNING
    Result<AllocationState> wait_for_allocation(const std::string& slurm_id,
                                                 StatusCallback cb);

    // Submit a new idle allocation, wait for RUNNING, return state
    Result<AllocationState> allocate(const SlurmDefaults& profile, StatusCallback cb);

    // Mark allocation as busy with a job (thread-safe)
    void assign_job(const std::string& slurm_id, const std::string& job_id);

    // Mark allocation as idle (job finished)
    void release_job(const std::string& slurm_id);

    // Cancel and remove an allocation
    void deallocate(const std::string& slurm_id, StatusCallback cb);

    // Cancel all idle allocations
    void deallocate_all_idle(StatusCallback cb);

    // Cancel idle allocations whose remaining time can't fit any configured job.
    void reap_expired_idle(const Config& config, StatusCallback cb = nullptr);

    const std::vector<AllocationState>& allocations() const { return state_.allocations; }
    ProjectState& state() { return state_; }

    // Persist state to disk
    void persist();

    // Resolve SLURM profile: job overrides > project > global
    SlurmDefaults resolve_profile(const std::string& job_name) const;

    // Parse "HH:MM:SS" to minutes
    static int parse_time_minutes(const std::string& time_str);

    // SLURM query result (state + node)
    struct SlurmQueryResult {
        std::string state;
        std::string node;
    };

    // Wait outcome for allocation polling
    struct WaitOutcome {
        enum Status { RUNNING, DEAD, PENDING } status;
        AllocationState alloc;
        std::string error;
    };

private:
    const Config& config_;
    SSHConnection& dtn_;
    SSHConnection& login_;
    StateStore& store_;
    ProjectState state_;
    std::string username_;
    std::mutex alloc_mutex_;  // guards find_free + assign_job atomicity

    // Internal: find a free allocation (caller must hold alloc_mutex_)
    AllocationState* find_free_unlocked(int required_minutes, const SlurmDefaults& required_resources,
                                        StatusCallback cb = nullptr);

    // Clear stale active_job_id references on allocations
    void clear_stale_job_refs(StatusCallback cb = nullptr);

    // Convert AllocationState to SlurmDefaults for resource comparison
    SlurmDefaults alloc_to_slurm_defaults(const AllocationState& alloc) const;

    // Query SLURM for allocation state (with retry on transient failure)
    SlurmQueryResult query_alloc_slurm(const std::string& slurm_id);

    // Remove an allocation from state and persist
    void remove_allocation(const std::string& slurm_id);

    // Single poll iteration for wait_for_allocation
    WaitOutcome check_alloc_state(const std::string& slurm_id, StatusCallback cb);

    int remaining_minutes(const AllocationState& alloc) const;
    std::string generate_alloc_script(const SlurmDefaults& profile) const;
    std::string persistent_base() const;
    std::string container_cache() const;
};
