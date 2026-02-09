#include "sync_manager.hpp"
#include "gitignore.hpp"
#include <core/config.hpp>
#include <core/credentials.hpp>
#include <fmt/format.h>
#include <filesystem>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

static std::string get_cluster_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "unknown";
}

SyncManager::SyncManager(const Config& config, SSHConnection& dtn)
    : config_(config), dtn_(dtn), username_(get_cluster_username()) {
}

std::vector<SyncManifestEntry> SyncManager::build_local_manifest() {
    std::vector<SyncManifestEntry> manifest;

    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();

    for (const auto& f : files) {
        auto rel = parser.get_relative_path(f);
        std::string rel_str = rel.string();

        // Skip rodata directory (handled separately)
        if (rel_str.substr(0, 7) == "rodata/" || rel_str == "rodata") {
            continue;
        }

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

    // Also add rodata files
    const auto& rodata = config_.project().rodata;
    for (const auto& [label, local_path] : rodata) {
        fs::path src = local_path;
        if (src.is_relative()) {
            src = config_.project_dir() / src;
        }
        if (!fs::exists(src) || !fs::is_directory(src)) continue;

        for (const auto& entry_fs : fs::recursive_directory_iterator(src)) {
            if (!entry_fs.is_regular_file()) continue;
            auto rel = fs::relative(entry_fs.path(), src);

            SyncManifestEntry entry;
            entry.path = "rodata/" + label + "/" + rel.string();

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
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";

    // Create scratch on compute node
    if (cb) cb("Creating scratch directory...");
    dtn_.run(fmt::format("ssh {} {} 'mkdir -p {}'", ssh_opts, compute_node, scratch_path));

    // Collect code files
    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();

    std::vector<fs::path> code_files;
    for (const auto& f : files) {
        auto rel = parser.get_relative_path(f);
        std::string rel_str = rel.string();
        if (rel_str.substr(0, 7) != "rodata/" && rel_str != "rodata") {
            code_files.push_back(f);
        }
    }

    if (cb) cb(fmt::format("Staging {} code files...", code_files.size()));

    // Upload code to DTN temp, then tar to compute node
    std::string dtn_tmp = fmt::format("/tmp/tccp_sync_{}", std::time(nullptr));
    std::string code_tmp = dtn_tmp + "/code";

    // Create dirs on DTN
    std::set<std::string> dirs;
    for (const auto& f : code_files) {
        auto rel = parser.get_relative_path(f);
        auto parent = rel.parent_path();
        if (!parent.empty() && parent != ".") {
            dirs.insert(code_tmp + "/" + parent.string());
        }
    }
    std::string mkdir_cmd = "mkdir -p " + code_tmp;
    for (const auto& d : dirs) {
        mkdir_cmd += " " + d;
    }
    dtn_.run(mkdir_cmd);

    // Upload files to DTN
    for (const auto& f : code_files) {
        auto rel = parser.get_relative_path(f);
        dtn_.upload(f, code_tmp + "/" + rel.string());
    }

    // Tar from DTN to compute node
    if (cb) cb("Transferring code to compute node...");
    dtn_.run(fmt::format("cd {}/code && tar cf - . | ssh {} {} 'cd {} && tar xf -'",
                         dtn_tmp, ssh_opts, compute_node, scratch_path));

    // Rodata
    const auto& rodata = proj.rodata;
    if (!rodata.empty()) {
        if (cb) cb("Staging rodata...");
        for (const auto& [label, local_path] : rodata) {
            std::string dest = dtn_tmp + "/rodata/" + label;
            dtn_.run("mkdir -p " + dest);

            fs::path src = local_path;
            if (src.is_relative()) src = config_.project_dir() / src;
            if (!fs::exists(src) || !fs::is_directory(src)) continue;

            for (const auto& entry : fs::recursive_directory_iterator(src)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), src);
                std::string remote = dest + "/" + rel.string();
                auto parent = rel.parent_path();
                if (!parent.empty() && parent != ".") {
                    dtn_.run("mkdir -p " + dest + "/" + parent.string());
                }
                dtn_.upload(entry.path(), remote);
            }
        }

        if (cb) cb("Transferring rodata to compute node...");
        dtn_.run(fmt::format("cd {} && tar cf - rodata | ssh {} {} 'cd {} && tar xf -'",
                             dtn_tmp, ssh_opts, compute_node, scratch_path));
    }

    // Clean up DTN temp
    dtn_.run("rm -rf " + dtn_tmp);

    if (cb) cb("Full sync complete");
}

void SyncManager::incremental_sync(const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::vector<std::string>& changed_files,
                                    const std::vector<std::string>& deleted_files,
                                    StatusCallback cb) {
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";
    const auto& proj = config_.project();

    // Delete removed files on remote
    if (!deleted_files.empty()) {
        for (const auto& f : deleted_files) {
            dtn_.run(fmt::format("ssh {} {} 'rm -f {}/{}'",
                                 ssh_opts, compute_node, scratch_path, f));
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

    // Separate code files and rodata files
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

    for (const auto& rel_path : changed_files) {
        fs::path local_file;

        if (rel_path.substr(0, 7) == "rodata/") {
            // rodata/label/rest → find the local source
            auto parts = rel_path.substr(7); // label/rest
            auto slash = parts.find('/');
            if (slash == std::string::npos) continue;
            std::string label = parts.substr(0, slash);
            std::string rest = parts.substr(slash + 1);

            auto it = proj.rodata.find(label);
            if (it == proj.rodata.end()) continue;
            fs::path src = it->second;
            if (src.is_relative()) src = config_.project_dir() / src;
            local_file = src / rest;
        } else {
            local_file = config_.project_dir() / rel_path;
        }

        if (fs::exists(local_file)) {
            dtn_.upload(local_file, dtn_tmp + "/" + rel_path);
        }
    }

    // Tar delta to compute node
    dtn_.run(fmt::format("cd {} && tar cf - . | ssh {} {} 'cd {} && tar xf -'",
                         dtn_tmp, ssh_opts, compute_node, scratch_path));

    dtn_.run("rm -rf " + dtn_tmp);

    if (cb) cb(fmt::format("Incremental sync: {} files updated", changed_files.size()));
}

void SyncManager::sync_to_scratch(const std::string& compute_node,
                                   const std::string& scratch_path,
                                   ProjectState& state,
                                   StatusCallback cb) {
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";

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
                                          ssh_opts, compute_node, state.last_sync_scratch));
        if (check.stdout_data.find("YES") == std::string::npos) {
            can_reuse = false;
        }
    }

    if (can_reuse && scratch_path != state.last_sync_scratch) {
        // Copy previous scratch to new scratch as starting point
        if (cb) cb("Reusing files from previous sync...");
        dtn_.run(fmt::format("ssh {} {} 'mkdir -p {} && cp -a {}/. {}/'",
                             ssh_opts, compute_node, scratch_path,
                             state.last_sync_scratch, scratch_path));

        // Diff manifests
        auto changed = diff_manifests(local_manifest, state.last_sync_manifest);

        // Find deleted files
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
}
