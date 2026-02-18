#include "resource_spec.hpp"
#include <managers/gpu_discovery.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>

std::string format_gpu_gres(const std::string& gpu_type, int gpu_count) {
    if (gpu_count <= 0 || gpu_type.empty()) return "";
    // Strip variant suffix: "a100-40gb" → "a100" (SLURM only knows the base GRES type)
    std::string base = gpu_type;
    const auto* variant = find_variant_by_id(gpu_type);
    if (variant) base = variant->base_type;
    return fmt::format("gpu:{}:{}", base, gpu_count);
}

std::string generate_sbatch_resources(const SlurmDefaults& profile) {
    std::string s;

    // Default partition: "gpu" if GPUs requested, "batch" otherwise
    std::string partition = profile.partition;
    if (partition.empty()) {
        bool has_gpu = profile.gpu_count > 0 || !profile.gpu_type.empty();
        partition = has_gpu ? "gpu" : "batch";
    }
    s += fmt::format("#SBATCH --partition={}\n", partition);

    int nodes = profile.nodes > 0 ? profile.nodes : 1;
    s += fmt::format("#SBATCH --nodes={}\n", nodes);

    int cpus = profile.cpus_per_task > 0 ? profile.cpus_per_task : 1;
    s += fmt::format("#SBATCH --cpus-per-task={}\n", cpus);

    std::string mem = profile.memory.empty() ? "4G" : profile.memory;
    s += fmt::format("#SBATCH --mem={}\n", mem);

    std::string gres = format_gpu_gres(profile.gpu_type, profile.gpu_count);
    if (!gres.empty()) {
        s += fmt::format("#SBATCH --gres={}\n", gres);
    }

    if (!profile.node_constraint.empty()) {
        s += fmt::format("#SBATCH -w {}[001-999]\n", profile.node_constraint);
    }

    if (!profile.exclude_nodes.empty()) {
        s += fmt::format("#SBATCH --exclude={}\n", profile.exclude_nodes);
    }

    if (!profile.mail_type.empty() && profile.mail_type != "NONE") {
        s += fmt::format("#SBATCH --mail-type={}\n", profile.mail_type);
    }

    return s;
}

int parse_memory_mb(const std::string& mem_str) {
    if (mem_str.empty()) return 0;

    // Find where the numeric part ends
    size_t i = 0;
    while (i < mem_str.size() && (std::isdigit(mem_str[i]) || mem_str[i] == '.')) {
        i++;
    }
    if (i == 0) return 0;

    double value = std::stod(mem_str.substr(0, i));
    std::string suffix = mem_str.substr(i);

    // Normalize suffix to uppercase
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::toupper);

    if (suffix.empty() || suffix == "M" || suffix == "MB") {
        return static_cast<int>(value);
    } else if (suffix == "G" || suffix == "GB") {
        return static_cast<int>(value * 1024);
    } else if (suffix == "T" || suffix == "TB") {
        return static_cast<int>(value * 1024 * 1024);
    }
    return static_cast<int>(value); // assume MB
}

bool resources_compatible(const SlurmDefaults& alloc_resources,
                          const SlurmDefaults& job_requirements) {
    // Partition must match (if job specifies one)
    if (!job_requirements.partition.empty()) {
        std::string alloc_part = alloc_resources.partition.empty() ? "" : alloc_resources.partition;
        if (alloc_part != job_requirements.partition) return false;
    }

    // CPUs: allocation must have >= job requirement
    int alloc_cpus = alloc_resources.cpus_per_task > 0 ? alloc_resources.cpus_per_task : 1;
    int job_cpus = job_requirements.cpus_per_task > 0 ? job_requirements.cpus_per_task : 1;
    if (alloc_cpus < job_cpus) return false;

    // Memory: allocation must have >= job requirement
    int alloc_mem = parse_memory_mb(alloc_resources.memory.empty() ? "4G" : alloc_resources.memory);
    int job_mem = parse_memory_mb(job_requirements.memory.empty() ? "4G" : job_requirements.memory);
    if (alloc_mem < job_mem) return false;

    // Nodes: allocation must have >= job requirement
    int alloc_nodes = alloc_resources.nodes > 0 ? alloc_resources.nodes : 1;
    int job_nodes = job_requirements.nodes > 0 ? job_requirements.nodes : 1;
    if (alloc_nodes < job_nodes) return false;

    // GPUs: allocation must have matching type and >= count
    int job_gpus = job_requirements.gpu_count;
    int alloc_gpus = alloc_resources.gpu_count;
    if (job_gpus > 0) {
        if (alloc_gpus < job_gpus) return false;
        // GPU type must match if job requires specific type
        if (!job_requirements.gpu_type.empty() &&
            alloc_resources.gpu_type != job_requirements.gpu_type) {
            return false;
        }
    }

    return true;
}
