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

    // Build manifest of all local files to sync (project + rodata + env)
    std::vector<SyncManifestEntry> build_local_manifest();

    // Diff current manifest against previous, returning changed and deleted paths
    static void diff_manifests(const std::vector<SyncManifestEntry>& current,
                               const std::vector<SyncManifestEntry>& previous,
                               std::vector<std::string>& changed,
                               std::vector<std::string>& deleted);

    // Pipe a local tar through DTN to compute node, extract, and verify probe.
    // Returns true if probe_file MD5 matches expected_md5 after extraction.
    bool pipe_tar_to_node(const std::filesystem::path& tar_path,
                          const std::string& compute_node,
                          const std::string& scratch_path,
                          const std::string& probe_file,
                          const std::string& expected_md5);

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
                          const std::string& expected_md5,
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

    // Verify scratch integrity: probe file MD5 matches AND remote file count
    // is at least as large as the manifest. Catches both corruption and
    // partial extraction from previous syncs.
    bool verify_scratch_integrity(const std::string& compute_node,
                                  const std::string& scratch_path,
                                  const std::string& probe_file,
                                  const std::string& expected_md5,
                                  size_t expected_file_count);

    // Extract a 32-char hex MD5 hash from noisy command output.
    static std::string parse_md5_from_output(const std::string& output);
};
