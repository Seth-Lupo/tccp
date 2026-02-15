#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <ssh/connection.hpp>
#include <core/types.hpp>

struct CacheItem {
    std::string path;       // absolute path on remote
    int64_t size_bytes;     // from du
    int64_t mtime_epoch;    // seconds since epoch
    enum Type { VENV, CONTAINER } type;
};

class CacheManager {
public:
    CacheManager(SSHConnection& dtn, const std::string& username);

    // Evict least-recently-used items if total ~/tccp/ usage exceeds soft cap.
    // current_project: project name whose venv is in-use (never evicted)
    // current_sif: SIF filename currently in use (never evicted)
    void ensure_within_cap(const std::string& current_project,
                           const std::string& current_sif,
                           StatusCallback cb);

    // Touch mtime on the SIF and project env sentinel to mark as recently used.
    void touch_used(const std::string& sif_path,
                    const std::string& project_env_path);

private:
    SSHConnection& dtn_;
    std::string username_;
    std::string tccp_home_;  // /cluster/home/{user}/tccp
};
