#pragma once

#include <string>
#include <vector>
#include <map>
#include <ssh/connection.hpp>

struct SlurmJobInfo {
    std::string state;  // RUNNING, PENDING, COMPLETED, FAILED, CANCELLED, ""
    std::string node;
};

// Query single allocation (with retry on transient failure)
SlurmJobInfo query_slurm_state(SSHConnection& login, const std::string& slurm_id);

// Batch query multiple allocations (single SSH call)
std::map<std::string, SlurmJobInfo> query_slurm_batch(SSHConnection& login,
                                                       const std::vector<std::string>& slurm_ids);
