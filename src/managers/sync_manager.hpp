#pragma once

#include <string>
#include <vector>
#include <filesystem>
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

    // Manifest cache to avoid rescanning unchanged directories
    std::vector<SyncManifestEntry> cached_manifest_;
    int64_t cached_manifest_mtime_ = 0;

    std::vector<SyncManifestEntry> build_local_manifest();
    std::vector<SyncManifestEntry> get_remote_manifest(
        const std::string& compute_node,
        const std::string& scratch_path);
    std::vector<std::string> diff_manifests(
        const std::vector<SyncManifestEntry>& local,
        const std::vector<SyncManifestEntry>& remote);

    // Collect all files to sync (project + rodata + env file)
    std::vector<std::pair<std::filesystem::path, std::string>> collect_sync_files();

    // Pipe a local tar archive through DTN to compute node (no DTN disk usage)
    void pipe_tar_to_node(const std::filesystem::path& tar_path,
                          const std::string& compute_node,
                          const std::string& scratch_path);

    void full_sync(const std::string& compute_node,
                   const std::string& scratch_path,
                   StatusCallback cb);

    void incremental_sync(const std::string& compute_node,
                          const std::string& scratch_path,
                          const std::vector<std::string>& changed_files,
                          const std::vector<std::string>& deleted_files,
                          StatusCallback cb);
};
