#include "sync_manager.hpp"
#include "gitignore.hpp"
#include <core/config.hpp>
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>
#include <filesystem>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

SyncManager::SyncManager(const Config& config, SSHConnection& dtn)
    : config_(config), dtn_(dtn), username_(get_cluster_username()) {
}

std::vector<SyncManifestEntry> SyncManager::build_local_manifest() {
    // Check project directory mtime to see if anything changed
    try {
        auto proj_mtime = fs::last_write_time(config_.project_dir()).time_since_epoch().count();
        if (!cached_manifest_.empty() && proj_mtime == cached_manifest_mtime_) {
            // Nothing changed, return cached manifest
            return cached_manifest_;
        }
        cached_manifest_mtime_ = proj_mtime;
    } catch (...) {
        // If mtime check fails, rebuild manifest
    }

    std::vector<SyncManifestEntry> manifest;

    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();

    for (const auto& f : files) {
        auto rel = parser.get_relative_path(f);
        std::string rel_str = rel.string();

        SyncManifestEntry entry;
        entry.path = rel_str;

        try {
            auto ftime = fs::last_write_time(f);
            entry.mtime = ftime.time_since_epoch().count();
            entry.size = static_cast<int64_t>(fs::file_size(f));
        } catch (...) {
            entry.mtime = 0;
            entry.size = 0;
        }

        manifest.push_back(entry);
    }

    // Add rodata files (maintaining exact directory structure)
    const auto& rodata = config_.project().rodata;
    for (const auto& rodata_path : rodata) {
        fs::path src = rodata_path;
        if (src.is_relative()) {
            src = config_.project_dir() / src;
        }
        if (!fs::exists(src)) continue;

        if (fs::is_directory(src)) {
            for (const auto& entry_fs : fs::recursive_directory_iterator(src)) {
                if (!entry_fs.is_regular_file()) continue;
                auto rel = fs::relative(entry_fs.path(), config_.project_dir());

                SyncManifestEntry entry;
                entry.path = rel.string();

                try {
                    auto ftime = fs::last_write_time(entry_fs.path());
                    entry.mtime = ftime.time_since_epoch().count();
                    entry.size = static_cast<int64_t>(fs::file_size(entry_fs.path()));
                } catch (...) {
                    entry.mtime = 0;
                    entry.size = 0;
                }

                manifest.push_back(entry);
            }
        }
    }

    // Add env file if configured (bypasses gitignore)
    const auto& env_file = config_.project().env_file;
    if (!env_file.empty()) {
        fs::path env_path = env_file;
        if (env_path.is_relative()) {
            env_path = config_.project_dir() / env_path;
        }
        if (fs::exists(env_path) && fs::is_regular_file(env_path)) {
            auto rel = fs::relative(env_path, config_.project_dir());
            SyncManifestEntry entry;
            entry.path = rel.string();
            try {
                auto ftime = fs::last_write_time(env_path);
                entry.mtime = ftime.time_since_epoch().count();
                entry.size = static_cast<int64_t>(fs::file_size(env_path));
            } catch (...) {
                entry.mtime = 0;
                entry.size = 0;
            }
            // Avoid duplicates (in case it wasn't gitignored)
            bool already_in = false;
            for (const auto& m : manifest) {
                if (m.path == entry.path) { already_in = true; break; }
            }
            if (!already_in) {
                manifest.push_back(entry);
            }
        }
    }

    // Cache the manifest for next time
    cached_manifest_ = manifest;
    return manifest;
}

std::vector<std::string> SyncManager::diff_manifests(
    const std::vector<SyncManifestEntry>& local,
    const std::vector<SyncManifestEntry>& remote) {

    // Build remote lookup: path → (mtime, size)
    std::map<std::string, std::pair<int64_t, int64_t>> remote_map;
    for (const auto& e : remote) {
        remote_map[e.path] = {e.mtime, e.size};
    }

    // Find files that are new or changed
    std::vector<std::string> changed;
    for (const auto& e : local) {
        auto it = remote_map.find(e.path);
        if (it == remote_map.end()) {
            // New file
            changed.push_back(e.path);
        } else if (it->second.first != e.mtime || it->second.second != e.size) {
            // Changed file
            changed.push_back(e.path);
        }
    }

    return changed;
}

void SyncManager::full_sync(const std::string& compute_node,
                             const std::string& scratch_path,
                             StatusCallback cb) {
    const auto& proj = config_.project();

    // Create scratch on compute node
    if (cb) cb("Creating scratch directory...");
    dtn_.run(fmt::format("ssh {} {} 'mkdir -p {}'", SSH_OPTS, compute_node, scratch_path));

    // Collect code files
    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();

    std::vector<fs::path> project_files;
    for (const auto& f : files) {
        project_files.push_back(f);
    }

    if (cb) cb(fmt::format("Staging {} project files...", project_files.size()));

    // Upload files to DTN temp, then tar to compute node
    std::string dtn_tmp = fmt::format("/tmp/tccp_sync_{}", std::time(nullptr));

    // Create all necessary directories on DTN
    std::set<std::string> dirs;
    for (const auto& f : project_files) {
        auto rel = parser.get_relative_path(f);
        auto parent = rel.parent_path();
        if (!parent.empty() && parent != ".") {
            dirs.insert(dtn_tmp + "/" + parent.string());
        }
    }
    std::string mkdir_cmd = "mkdir -p " + dtn_tmp;
    for (const auto& d : dirs) {
        mkdir_cmd += " " + d;
    }
    dtn_.run(mkdir_cmd);

    // Upload all project files to DTN
    for (const auto& f : project_files) {
        auto rel = parser.get_relative_path(f);
        dtn_.upload(f, dtn_tmp + "/" + rel.string());
    }

    // Add rodata files
    const auto& rodata = proj.rodata;
    for (const auto& rodata_path : rodata) {
        fs::path src = rodata_path;
        if (src.is_relative()) src = config_.project_dir() / src;
        if (!fs::exists(src)) continue;

        if (fs::is_directory(src)) {
            for (const auto& entry : fs::recursive_directory_iterator(src)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), config_.project_dir());
                std::string remote = dtn_tmp + "/" + rel.string();
                auto parent = rel.parent_path();
                if (!parent.empty() && parent != ".") {
                    dtn_.run("mkdir -p " + dtn_tmp + "/" + parent.string());
                }
                dtn_.upload(entry.path(), remote);
            }
        }
    }

    // Add env file (bypasses gitignore, may not be in collect_files)
    const auto& env_file = proj.env_file;
    if (!env_file.empty()) {
        fs::path env_path = env_file;
        if (env_path.is_relative()) env_path = config_.project_dir() / env_path;
        if (fs::exists(env_path) && fs::is_regular_file(env_path)) {
            auto rel = fs::relative(env_path, config_.project_dir());
            auto parent = rel.parent_path();
            if (!parent.empty() && parent != ".") {
                dtn_.run("mkdir -p " + dtn_tmp + "/" + parent.string());
            }
            dtn_.upload(env_path, dtn_tmp + "/" + rel.string());
        }
    }

    // Tar from DTN to compute node
    if (cb) cb("Transferring files to compute node...");
    dtn_.run(fmt::format("cd {} && tar cf - . | ssh {} {} 'cd {} && tar xf -'",
                         dtn_tmp, SSH_OPTS, compute_node, scratch_path));

    // Clean up DTN temp
    dtn_.run("rm -rf " + dtn_tmp);

    if (cb) cb("Full sync complete");
}

void SyncManager::incremental_sync(const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::vector<std::string>& changed_files,
                                    const std::vector<std::string>& deleted_files,
                                    StatusCallback cb) {
    const auto& proj = config_.project();

    // Delete removed files on remote
    if (!deleted_files.empty()) {
        for (const auto& f : deleted_files) {
            dtn_.run(fmt::format("ssh {} {} 'rm -f {}/{}'",
                                 SSH_OPTS, compute_node, scratch_path, f));
        }
    }

    if (changed_files.empty()) {
        if (cb) cb("No files changed");
        return;
    }

    if (cb) cb(fmt::format("Uploading {} changed files...", changed_files.size()));

    // Upload changed files to DTN temp
    std::string dtn_tmp = fmt::format("/tmp/tccp_delta_{}", std::time(nullptr));
    GitignoreParser parser(config_.project_dir());

    // Create necessary directories on DTN
    std::set<std::string> dirs_needed;
    for (const auto& rel_path : changed_files) {
        auto parent = fs::path(rel_path).parent_path();
        if (!parent.empty() && parent != ".") {
            dirs_needed.insert(dtn_tmp + "/" + parent.string());
        }
    }
    std::string mkdir_cmd = "mkdir -p " + dtn_tmp;
    for (const auto& d : dirs_needed) {
        mkdir_cmd += " " + d;
    }
    dtn_.run(mkdir_cmd);

    // Upload changed files (paths are already relative to project dir)
    for (const auto& rel_path : changed_files) {
        fs::path local_file = config_.project_dir() / rel_path;

        if (fs::exists(local_file)) {
            dtn_.upload(local_file, dtn_tmp + "/" + rel_path);
        }
    }

    // Tar delta to compute node
    dtn_.run(fmt::format("cd {} && tar cf - . | ssh {} {} 'cd {} && tar xf -'",
                         dtn_tmp, SSH_OPTS, compute_node, scratch_path));

    dtn_.run("rm -rf " + dtn_tmp);

    if (cb) cb(fmt::format("Incremental sync: {} files updated", changed_files.size()));
}

void SyncManager::sync_to_scratch(const std::string& compute_node,
                                   const std::string& scratch_path,
                                   ProjectState& state,
                                   StatusCallback cb) {

    // Build current local manifest
    auto local_manifest = build_local_manifest();

    // Check if we can reuse a previous sync on this node
    bool can_reuse = !state.last_sync_node.empty() &&
                     state.last_sync_node == compute_node &&
                     !state.last_sync_scratch.empty() &&
                     !state.last_sync_manifest.empty();

    if (can_reuse) {
        // Verify old scratch still exists
        auto check = dtn_.run(fmt::format("ssh {} {} 'test -d {} && echo YES || echo NO'",
                                          SSH_OPTS, compute_node, state.last_sync_scratch));
        if (check.stdout_data.find("YES") == std::string::npos) {
            can_reuse = false;
        }
    }

    if (can_reuse && scratch_path != state.last_sync_scratch) {
        // Copy previous scratch to new scratch as starting point.
        // Race guard: the old scratch may be deleted by poll/cleanup between
        // the exists-check above and this cp. If cp fails, fall back to full sync.
        if (cb) cb("Reusing files from previous sync...");
        auto cp_result = dtn_.run(fmt::format(
            "ssh {} {} 'mkdir -p {} && cp -a {}/. {}/ 2>/dev/null && echo CP_OK || echo CP_FAIL'",
            SSH_OPTS, compute_node, scratch_path,
            state.last_sync_scratch, scratch_path));

        // Delete old scratch regardless (it's either copied or gone)
        dtn_.run(fmt::format("ssh {} {} 'rm -rf {}'",
                             SSH_OPTS, compute_node, state.last_sync_scratch));

        if (cp_result.stdout_data.find("CP_FAIL") != std::string::npos) {
            // Old scratch was deleted (race with cleanup) — full sync instead
            if (cb) cb("Previous scratch gone, doing full sync...");
            full_sync(compute_node, scratch_path, cb);
        } else {
            // Copy succeeded — only sync changed files
            auto changed = diff_manifests(local_manifest, state.last_sync_manifest);

            std::set<std::string> local_set;
            for (const auto& e : local_manifest) local_set.insert(e.path);
            std::vector<std::string> deleted;
            for (const auto& e : state.last_sync_manifest) {
                if (local_set.find(e.path) == local_set.end()) {
                    deleted.push_back(e.path);
                }
            }

            if (!changed.empty() || !deleted.empty()) {
                incremental_sync(compute_node, scratch_path, changed, deleted, cb);
            } else {
                if (cb) cb("All files up to date");
            }
        }
    } else if (can_reuse && scratch_path == state.last_sync_scratch) {
        // Same scratch path — just do incremental in place
        auto changed = diff_manifests(local_manifest, state.last_sync_manifest);

        std::set<std::string> local_set;
        for (const auto& e : local_manifest) local_set.insert(e.path);
        std::vector<std::string> deleted;
        for (const auto& e : state.last_sync_manifest) {
            if (local_set.find(e.path) == local_set.end()) {
                deleted.push_back(e.path);
            }
        }

        if (!changed.empty() || !deleted.empty()) {
            incremental_sync(compute_node, scratch_path, changed, deleted, cb);
        } else {
            if (cb) cb("All files up to date");
        }
    } else {
        // No previous sync — full upload
        full_sync(compute_node, scratch_path, cb);
    }

    // Update manifest state
    state.last_sync_manifest = local_manifest;
    state.last_sync_node = compute_node;
    state.last_sync_scratch = scratch_path;

    // Clean up stale scratch dirs from previous runs (crashes, edge cases).
    // scratch_path is /tmp/{user}/{project}/{job_id} — delete any siblings
    // under the same parent that aren't the current scratch.
    fs::path scratch_parent = fs::path(scratch_path).parent_path().string();
    std::string current_dir = fs::path(scratch_path).filename().string();
    if (!scratch_parent.empty() && !current_dir.empty()) {
        std::string cleanup_cmd = fmt::format(
            "ssh {} {} 'for d in {}/*/; do b=$(basename \"$d\"); "
            "[ \"$b\" != \"{}\" ] && rm -rf \"$d\"; done 2>/dev/null; true'",
            SSH_OPTS, compute_node, scratch_parent.string(), current_dir);
        dtn_.run(cleanup_cmd);
    }
}
