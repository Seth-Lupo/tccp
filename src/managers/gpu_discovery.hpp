#pragma once

#include <string>
#include <vector>
#include <ssh/connection.hpp>

// A single GPU resource entry from sinfo (one row per partition+gres combo)
struct GpuResource {
    std::string partition;
    std::string gpu_type;       // e.g. "a100", "v100", "rtx_a5000"
    int gpu_per_node = 0;       // GPUs available per node
    int avail_nodes = 0;        // nodes in idle/mix state (can accept work)
    int total_nodes = 0;        // total nodes with this resource
    int mem_mb = 0;             // memory per node in MB
    int cpus_per_node = 0;      // CPUs per node
};

// Result of partition selection
struct PartitionMatch {
    bool found = false;
    std::string partition;
    std::string gpu_type;       // resolved type (may differ from request if generic)
    int gpu_per_node = 0;
    std::string error;          // human-readable reason if !found
};

// ── Query functions ──────────────────────────────────────────

// Discover all GPU resources the current user can access.
// Uses `sinfo` filtered by user-accessible partitions.
std::vector<GpuResource> discover_gpu_resources(SSHConnection& login);

// Discover which partitions the user is permitted to submit to.
// Uses sacctmgr association data; falls back to sinfo partition list.
std::vector<std::string> discover_user_partitions(SSHConnection& login,
                                                   const std::string& username);

// ── Selection functions (pure, no SSH) ───────────────────────

// Find the best partition for a GPU request.
//   gpu_type:  requested type (e.g. "a100"), or "" for any GPU
//   gpu_count: number of GPUs needed (per node)
//   user_partitions: allowed partitions (empty = no filter)
//
// Selection rules:
//   1. Partition must be in user_partitions (if non-empty)
//   2. Partition must have the requested gpu_type (exact match)
//   3. Partition must have >= gpu_count GPUs per node
//   4. Prefer partitions with available (idle/mix) nodes
//   5. Among ties, prefer the partition with fewest GPUs per node
//      (don't waste a 8-GPU node on a 1-GPU job)
PartitionMatch find_gpu_partition(const std::vector<GpuResource>& resources,
                                   const std::string& gpu_type,
                                   int gpu_count,
                                   const std::vector<std::string>& user_partitions = {});

// Convenience: full discovery + selection in one call.
// If partition is already explicitly set (non-empty, not "batch"),
// returns it unchanged.
PartitionMatch resolve_gpu_partition(SSHConnection& login,
                                      const std::string& username,
                                      const std::string& current_partition,
                                      const std::string& gpu_type,
                                      int gpu_count);
