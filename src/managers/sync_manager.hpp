#pragma once

#include <string>
#include <vector>
#include <set>
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

    // Build manifest of all local files to sync (project + rodata + env)
    std::vector<SyncManifestEntry> build_local_manifest();

    // Diff current manifest against previous, returning changed and deleted paths
    static void diff_manifests(const std::vector<SyncManifestEntry>& current,
                               const std::vector<SyncManifestEntry>& previous,
                               std::vector<std::string>& changed,
                               std::vector<std::string>& deleted);

    // Pipe a local tar through DTN to compute node, extract, and verify probe.
    // Returns true if probe_file was found on remote after extraction.
    bool pipe_tar_to_node(const std::filesystem::path& tar_path,
                          const std::string& compute_node,
                          const std::string& scratch_path,
                          const std::string& probe_file);

    // Send all local files to compute node. Returns true if verified.
    bool full_sync(const std::string& compute_node,
                   const std::string& scratch_path,
                   const std::vector<SyncManifestEntry>& manifest,
                   const std::string& probe_file,
                   StatusCallback cb);

    // Send only changed files and delete removed files. Returns true if verified.
    bool incremental_sync(const std::string& compute_node,
                          const std::string& scratch_path,
                          const std::vector<std::string>& changed_files,
                          const std::vector<std::string>& deleted_files,
                          const std::string& probe_file,
                          StatusCallback cb);

    // Try to reuse a previous scratch dir on the same node (mv or cp)
    void reuse_previous_scratch(const std::string& compute_node,
                                const std::string& scratch_path,
                                const std::string& old_scratch,
                                const std::set<std::string>& active_scratches,
                                StatusCallback cb);

    // Remove scratch dirs not owned by any active job (single SSH command)
    void cleanup_stale_scratches(const std::string& compute_node,
                                 const std::string& scratch_path,
                                 const std::set<std::string>& active_scratches);
};
