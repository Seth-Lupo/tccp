#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct AllocationState {
    std::string slurm_id;
    std::string node;                // compute node hostname
    std::string start_time;          // ISO timestamp
    int duration_minutes = 240;      // total allocation time (default 4h)
    std::string project_name;        // which project owns this allocation
    std::string active_job_id;       // "" if idle (full job_id with timestamp)
};

struct ClusterAllocations {
    std::vector<AllocationState> allocations;
};

class AllocationStore {
public:
    explicit AllocationStore(const std::string& cluster_name);

    ClusterAllocations load();
    void save(const ClusterAllocations& allocs);

    const fs::path& path() const { return store_path_; }

private:
    std::string cluster_name_;
    fs::path store_path_;  // ~/.tccp/allocations/<cluster>.yaml
};
