#pragma once

#include <string>
#include <vector>
#include <core/config.hpp>
#include <ssh/connection.hpp>
#include "state_store.hpp"

class SyncManager {
public:
    SyncManager(const Config& config, SSHConnection& dtn);

    // Sync local project files to compute node scratch.
    // Reuses files from previous sync if on same node.
    void sync_to_scratch(const std::string& compute_node,
                         const std::string& scratch_path,
                         ProjectState& state,
                         StatusCallback cb);

private:
    const Config& config_;
    SSHConnection& dtn_;
    std::string username_;

    std::vector<SyncManifestEntry> build_local_manifest();
    std::vector<std::string> diff_manifests(
        const std::vector<SyncManifestEntry>& local,
        const std::vector<SyncManifestEntry>& remote);

    void full_sync(const std::string& compute_node,
                   const std::string& scratch_path,
                   StatusCallback cb);

    void incremental_sync(const std::string& compute_node,
                          const std::string& scratch_path,
                          const std::vector<std::string>& changed_files,
                          const std::vector<std::string>& deleted_files,
                          StatusCallback cb);
};
