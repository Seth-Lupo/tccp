#pragma once

#include <string>
#include <vector>
#include <ssh/connection.hpp>

// GPU variant: maps node-name prefixes to specific GPU sub-types that share
// the same SLURM GRES base type.  Add a row here for each new node class.
struct GpuVariant {
    std::string id;           // "a100-40gb" — user-facing, used in gpu_type config
    std::string base_type;    // "a100" — matches GRES from sinfo
    std::string node_prefix;  // "cc1gpu" — node hostname prefix
    int mem_gb;               // 40
    int tier;                 // 1 = cheapest (preferred by default)
};

// Easily extensible — add a row for new node types
inline const std::vector<GpuVariant>& gpu_variants() {
    static const std::vector<GpuVariant> variants = {
        {"a100-40gb", "a100", "cc1gpu", 40, 1},
        {"a100-80gb", "a100", "s1cmp",  80, 2},
    };
    return variants;
}

// Look up a variant by its user-facing ID (e.g. "a100-40gb").  Returns nullptr if not found.
const GpuVariant* find_variant_by_id(const std::string& id);

// Look up all variants that share a GRES base type (e.g. "a100").
std::vector<const GpuVariant*> find_variants_by_base(const std::string& base_type);

// A single GPU resource entry from sinfo (one row per partition+gres combo)
struct GpuResource {
    std::string partition;
    std::string gpu_type;       // e.g. "a100-40gb", "v100", "rtx_a5000"
    int gpu_per_node = 0;       // GPUs available per node
    int avail_nodes = 0;        // nodes in idle/mix state (can accept work)
    int total_nodes = 0;        // total nodes with this resource
    int mem_mb = 0;             // memory per node in MB
    int cpus_per_node = 0;      // CPUs per node
    std::string node_prefix;    // variant node prefix (e.g. "cc1gpu"), empty if no variant
};

// Result of partition selection
struct PartitionMatch {
    bool found = false;
    std::string partition;
    std::string gpu_type;       // resolved type (may differ from request if generic)
    int gpu_per_node = 0;
    std::string node_prefix;    // node prefix constraint for sbatch (empty = none)
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
