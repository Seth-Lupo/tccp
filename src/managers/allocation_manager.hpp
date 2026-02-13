#pragma once

#include <string>
#include <vector>
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

    // Find a free RUNNING allocation with enough time remaining and
    // compatible resources. Pass resolved profile for resource matching.
    // Optional callback for logging why each allocation is accepted/rejected.
    AllocationState* find_free(int required_minutes, const SlurmDefaults& required_resources,
                               StatusCallback cb = nullptr);

    // Find allocation by SLURM job ID (direct lookup, no resource matching)
    AllocationState* find_by_id(const std::string& slurm_id);

    // Find a PENDING allocation (submitted but no node yet)
    AllocationState* find_pending();

    // Wait for a specific allocation to reach RUNNING
    Result<AllocationState> wait_for_allocation(const std::string& slurm_id,
                                                 StatusCallback cb);

    // Submit a new idle allocation, wait for RUNNING, return state
    Result<AllocationState> allocate(const SlurmDefaults& profile, StatusCallback cb);

    // Mark allocation as busy with a job
    void assign_job(const std::string& slurm_id, const std::string& job_id);

    // Mark allocation as idle (job finished)
    void release_job(const std::string& slurm_id);

    // Cancel and remove an allocation
    void deallocate(const std::string& slurm_id, StatusCallback cb);

    // Cancel all idle allocations
    void deallocate_all_idle(StatusCallback cb);

    const std::vector<AllocationState>& allocations() const { return state_.allocations; }
    ProjectState& state() { return state_; }

    // Persist state to disk
    void persist();

    // Resolve SLURM profile: job overrides > project > global
    SlurmDefaults resolve_profile(const std::string& job_name) const;

    // Parse "HH:MM:SS" to minutes
    static int parse_time_minutes(const std::string& time_str);

private:
    const Config& config_;
    SSHConnection& dtn_;
    SSHConnection& login_;
    StateStore& store_;
    ProjectState state_;
    std::string username_;

    int remaining_minutes(const AllocationState& alloc) const;
    std::string generate_alloc_script(const SlurmDefaults& profile) const;
    std::string persistent_base() const;
    std::string container_cache() const;
};
