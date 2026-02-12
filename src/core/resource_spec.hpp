#pragma once

#include <string>
#include <core/types.hpp>

// Generate #SBATCH resource directives from a resolved SlurmDefaults profile.
// Produces lines for: partition, nodes, cpus-per-task, mem, gres, mail-type.
// Only emits --gres if gpu_count > 0 and gpu_type is non-empty.
std::string generate_sbatch_resources(const SlurmDefaults& profile);

// Format GPU gres string: "gpu:a100:4" or "" if no GPUs requested.
std::string format_gpu_gres(const std::string& gpu_type, int gpu_count);

// Check if an allocation's resources satisfy a job's requirements.
// Returns true if the allocation has >= CPUs, >= memory, matching partition,
// and compatible GPU configuration.
bool resources_compatible(const SlurmDefaults& alloc_resources,
                          const SlurmDefaults& job_requirements);

// Parse a memory string like "128G", "4096M", "4G" to megabytes.
// Returns 0 on parse failure.
int parse_memory_mb(const std::string& mem_str);
