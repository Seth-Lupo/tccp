#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>

namespace fs = std::filesystem;

struct AllocationState {
    std::string slurm_id;
    std::string node;               // compute node hostname
    std::string start_time;         // ISO timestamp
    int duration_minutes = 240;     // total allocation time (default 4h)
    std::string active_job_id;      // "" if idle

    // Resource specification (what was requested from SLURM)
    std::string partition;
    int nodes = 1;
    int cpus = 1;
    std::string memory = "4G";
    std::string gpu_type;
    int gpu_count = 0;
};

struct JobState {
    std::string job_id;
    std::string job_name;
    std::string alloc_slurm_id;     // allocation it ran on
    std::string compute_node;
    bool completed = false;
    bool canceled = false;          // true if manually killed via 'cancel'
    int exit_code = -1;
    std::string output_file;        // local capture path
    std::string scratch_path;       // /tmp/{user}/{project}/{job_id}

    // Init tracking
    bool init_complete = false;
    std::string init_error;

    // Output tracking
    bool output_returned = false;   // true if output was downloaded and remote cleaned

    // Timing
    std::string submit_time;        // ISO timestamp when job was submitted
    std::string start_time;         // ISO timestamp when job started running
    std::string end_time;           // ISO timestamp when job completed
};

struct SyncManifestEntry {
    std::string path;               // relative file path
    int64_t mtime = 0;
    int64_t size = 0;
};

struct ProjectState {
    std::vector<AllocationState> allocations;
    std::vector<JobState> jobs;
    std::vector<SyncManifestEntry> last_sync_manifest;
    std::string last_sync_node;
    std::string last_sync_scratch;
};

class StateStore {
public:
    explicit StateStore(const std::string& project_name);

    ProjectState load();
    void save(const ProjectState& state);

    const fs::path& path() const { return state_path_; }

private:
    fs::path state_path_;
};
