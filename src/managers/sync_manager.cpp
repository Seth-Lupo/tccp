#include "sync_manager.hpp"
#include "gitignore.hpp"
#include <core/config.hpp>
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <map>

namespace fs = std::filesystem;

SyncManager::SyncManager(const Config& config, SSHConnection& dtn)
    : config_(config), dtn_(dtn), username_(get_cluster_username()) {
}

// ── Manifest ───────────────────────────────────────────────

std::vector<SyncManifestEntry> SyncManager::build_local_manifest() {
    try {
        auto proj_mtime = fs::last_write_time(config_.project_dir()).time_since_epoch().count();
        if (!cached_manifest_.empty() && proj_mtime == cached_manifest_mtime_) {
            return cached_manifest_;
        }
        cached_manifest_mtime_ = proj_mtime;
    } catch (...) {}

    std::vector<SyncManifestEntry> manifest;
    std::set<std::string> seen;

    auto add_file = [&](const fs::path& abs_path, const std::string& rel) {
        if (!seen.insert(rel).second) return;
        SyncManifestEntry entry;
        entry.path = rel;
        try {
            entry.mtime = fs::last_write_time(abs_path).time_since_epoch().count();
            entry.size = static_cast<int64_t>(fs::file_size(abs_path));
        } catch (...) {
            entry.mtime = 0;
            entry.size = 0;
        }
        manifest.push_back(entry);
    };

    // Project files (respecting .gitignore)
    GitignoreParser parser(config_.project_dir());
    for (const auto& f : parser.collect_files()) {
        add_file(f, parser.get_relative_path(f).string());
    }

    // Rodata files
    for (const auto& rodata_path : config_.project().rodata) {
        fs::path src = rodata_path;
        if (src.is_relative()) src = config_.project_dir() / src;
        if (!fs::exists(src) || !fs::is_directory(src)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(src)) {
            if (!entry.is_regular_file()) continue;
            add_file(entry.path(), fs::relative(entry.path(), config_.project_dir()).string());
        }
    }

    // Env file (bypasses gitignore)
    const auto& env_file = config_.project().env;
    if (!env_file.empty()) {
        fs::path env_path = env_file;
        if (env_path.is_relative()) env_path = config_.project_dir() / env_path;
        if (fs::exists(env_path) && fs::is_regular_file(env_path)) {
            add_file(env_path, fs::relative(env_path, config_.project_dir()).string());
        }
    }

    cached_manifest_ = manifest;
    return manifest;
}

void SyncManager::diff_manifests(const std::vector<SyncManifestEntry>& current,
                                  const std::vector<SyncManifestEntry>& previous,
                                  std::vector<std::string>& changed,
                                  std::vector<std::string>& deleted) {
    std::map<std::string, std::pair<int64_t, int64_t>> prev_map;
    for (const auto& e : previous) {
        prev_map[e.path] = {e.mtime, e.size};
    }

    for (const auto& e : current) {
        auto it = prev_map.find(e.path);
        if (it == prev_map.end()) {
            changed.push_back(e.path);
        } else if (it->second.first != e.mtime || it->second.second != e.size) {
            changed.push_back(e.path);
        }
    }

    std::set<std::string> current_set;
    for (const auto& e : current) current_set.insert(e.path);
    for (const auto& e : previous) {
        if (current_set.find(e.path) == current_set.end()) {
            deleted.push_back(e.path);
        }
    }
}

// ── Tar + transfer ─────────────────────────────────────────

static fs::path create_local_tar(const fs::path& project_dir,
                                  const std::vector<std::string>& rel_paths) {
    fs::path tar_path = fs::temp_directory_path() / fmt::format("tccp_sync_{}.tar", std::time(nullptr));
    fs::path list_path = fs::temp_directory_path() / fmt::format("tccp_sync_{}.list", std::time(nullptr));

    {
        std::ofstream list(list_path);
        for (const auto& rel : rel_paths) {
            list << rel << "\n";
        }
    }

    std::string cmd = fmt::format("tar cf {} -C {} -T {}",
                                  tar_path.string(), project_dir.string(), list_path.string());
    std::system(cmd.c_str());
    fs::remove(list_path);
    return tar_path;
}

void SyncManager::pipe_tar_to_node(const fs::path& tar_path,
                                    const std::string& compute_node,
                                    const std::string& scratch_path) {
    const char* home = std::getenv("HOME");
    std::string key_path = home ? std::string(home) + "/.ssh/id_ed25519" : "";

    if (key_path.empty() || !fs::exists(key_path)) {
        throw std::runtime_error(
            "SSH key not found at ~/.ssh/id_ed25519 — run a job first to generate keys, "
            "or run ssh-keygen -t ed25519");
    }

    // Nested SSH: local → DTN → compute node.
    // The second hop uses the DTN's cluster-internal auth (GSSAPI/hostbased),
    // which compute nodes accept. ProxyJump doesn't work because compute nodes
    // reject direct pubkey auth from external keys.
    // -T: disable PTY on outer hop (binary-clean pipe)
    // exec: replace DTN shell immediately so .bashrc can't consume stdin
    std::string dtn_host = fmt::format("{}@{}", username_, config_.dtn().host);
    std::string cmd = fmt::format(
        "cat {} | ssh -T -i {} "
        "-o StrictHostKeyChecking=no -o BatchMode=yes "
        "{} 'exec ssh {} {} \"mkdir -p {} && cd {} && tar xf - 2>/dev/null\"'",
        tar_path.string(), key_path,
        dtn_host, SSH_OPTS, compute_node, scratch_path, scratch_path);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        throw std::runtime_error(fmt::format(
            "File sync failed (ssh exit code {}). Check SSH connectivity to DTN.", rc));
    }
}

// Verify that a specific file exists on the remote scratch dir.
// Uses the libssh2 connection (DTN hop) which we know works.
bool SyncManager::verify_remote_file(const std::string& compute_node,
                                      const std::string& scratch_path,
                                      const std::string& rel_path) {
    auto result = dtn_.run(fmt::format(
        "ssh {} {} 'test -f {}/{} && echo OK'",
        SSH_OPTS, compute_node, scratch_path, rel_path));
    return result.stdout_data.find("OK") != std::string::npos;
}

// ── Sync strategies ────────────────────────────────────────

void SyncManager::full_sync(const std::string& compute_node,
                             const std::string& scratch_path,
                             const std::vector<SyncManifestEntry>& manifest,
                             StatusCallback cb) {
    if (cb) cb(fmt::format("Syncing {} files to compute node...", manifest.size()));

    std::vector<std::string> rel_paths;
    rel_paths.reserve(manifest.size());
    for (const auto& e : manifest) rel_paths.push_back(e.path);

    fs::path tar_path = create_local_tar(config_.project_dir(), rel_paths);
    pipe_tar_to_node(tar_path, compute_node, scratch_path);
    fs::remove(tar_path);

    if (cb) cb("Full sync complete");
}

void SyncManager::incremental_sync(const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::vector<std::string>& changed_files,
                                    const std::vector<std::string>& deleted_files,
                                    StatusCallback cb) {
    if (!deleted_files.empty()) {
        std::string rm_cmd = "rm -f";
        for (const auto& f : deleted_files) {
            rm_cmd += " " + scratch_path + "/" + f;
        }
        dtn_.run(fmt::format("ssh {} {} '{}'", SSH_OPTS, compute_node, rm_cmd));
    }

    if (changed_files.empty()) {
        if (cb) cb("No files changed");
        return;
    }

    if (cb) cb(fmt::format("Syncing {} changed files...", changed_files.size()));

    std::vector<std::string> existing;
    for (const auto& rel : changed_files) {
        if (fs::exists(config_.project_dir() / rel)) {
            existing.push_back(rel);
        }
    }
    if (existing.empty()) return;

    fs::path tar_path = create_local_tar(config_.project_dir(), existing);
    pipe_tar_to_node(tar_path, compute_node, scratch_path);
    fs::remove(tar_path);

    if (cb) cb(fmt::format("Incremental sync: {} files updated", existing.size()));
}

// ── Scratch reuse + cleanup ────────────────────────────────

void SyncManager::reuse_previous_scratch(const std::string& compute_node,
                                          const std::string& scratch_path,
                                          const std::string& old_scratch,
                                          const std::set<std::string>& active_scratches,
                                          StatusCallback cb) {
    bool old_is_active = active_scratches.count(old_scratch) > 0;

    if (!old_is_active) {
        if (cb) cb("Reusing previous scratch...");
        dtn_.run(fmt::format(
            "ssh {} {} '"
            "if [ -d {} ]; then mv {} {}; else mkdir -p {}; fi'",
            SSH_OPTS, compute_node,
            old_scratch, old_scratch, scratch_path, scratch_path));
    } else {
        if (cb) cb("Copying files from previous scratch...");
        dtn_.run(fmt::format(
            "ssh {} {} '"
            "if [ -d {} ]; then "
            "mkdir -p {} && "
            "cd {} && "
            "tar cf - "
            "--exclude=cache --exclude=output --exclude=env "
            "--exclude=.tccp-tmp --exclude=\"*.sock\" --exclude=\"*.log\" "
            "--exclude=tccp_run.sh . 2>/dev/null | "
            "(cd {} && tar xf -); "
            "else mkdir -p {}; fi'",
            SSH_OPTS, compute_node,
            old_scratch,
            scratch_path, old_scratch,
            scratch_path, scratch_path));
    }
}

void SyncManager::cleanup_stale_scratches(const std::string& compute_node,
                                           const std::string& scratch_path,
                                           const std::set<std::string>& active_scratches) {
    fs::path scratch_parent = fs::path(scratch_path).parent_path().string();
    if (scratch_parent.empty()) return;

    auto ls_result = dtn_.run(fmt::format(
        "ssh {} {} 'ls -d {}/*/ 2>/dev/null || true'",
        SSH_OPTS, compute_node, scratch_parent.string()));

    if (ls_result.stdout_data.empty()) return;

    std::istringstream dirs(ls_result.stdout_data);
    std::string dir_path;
    std::vector<std::string> to_delete;

    while (std::getline(dirs, dir_path)) {
        while (!dir_path.empty() && (dir_path.back() == '/' || dir_path.back() == '\n' || dir_path.back() == '\r'))
            dir_path.pop_back();
        if (dir_path.empty()) continue;
        if (dir_path == scratch_path) continue;
        if (active_scratches.count(dir_path)) continue;
        to_delete.push_back(dir_path);
    }

    if (!to_delete.empty()) {
        std::string rm_cmd = "rm -rf";
        for (const auto& d : to_delete) rm_cmd += " " + d;
        dtn_.run(fmt::format("ssh {} {} '{}'", SSH_OPTS, compute_node, rm_cmd));
    }
}

// ── Orchestrator ───────────────────────────────────────────

void SyncManager::sync_to_scratch(const std::string& compute_node,
                                   const std::string& scratch_path,
                                   ProjectState& state,
                                   StatusCallback cb) {
    auto local_manifest = build_local_manifest();

    if (local_manifest.empty()) {
        throw std::runtime_error("No files to sync — check project directory and .gitignore");
    }

    // Collect active scratches (never delete these)
    std::set<std::string> active_scratches;
    for (const auto& js : state.jobs) {
        if (!js.completed && !js.scratch_path.empty()) {
            active_scratches.insert(js.scratch_path);
        }
    }

    // Pick a file from the manifest to use as a verification probe
    std::string probe_file = local_manifest[0].path;

    // Try to reuse previous scratch on the same node
    bool reused = false;
    if (!state.last_sync_node.empty() &&
        state.last_sync_node == compute_node &&
        !state.last_sync_scratch.empty() &&
        state.last_sync_scratch != scratch_path) {
        reuse_previous_scratch(compute_node, scratch_path,
                               state.last_sync_scratch, active_scratches, cb);
        reused = true;
    }

    // Decide sync strategy: incremental or full.
    // Use incremental only if we have a previous manifest from this node
    // AND we can verify that a known file actually exists on the remote.
    bool did_incremental = false;
    if (reused &&
        !state.last_sync_manifest.empty() &&
        state.last_sync_node == compute_node &&
        verify_remote_file(compute_node, scratch_path, probe_file)) {

        std::vector<std::string> changed, deleted;
        diff_manifests(local_manifest, state.last_sync_manifest, changed, deleted);

        if (!changed.empty() || !deleted.empty()) {
            incremental_sync(compute_node, scratch_path, changed, deleted, cb);
        } else {
            if (cb) cb("All files up to date");
        }
        did_incremental = true;
    }

    if (!did_incremental) {
        full_sync(compute_node, scratch_path, local_manifest, cb);
    }

    // Verify the sync actually worked
    if (!verify_remote_file(compute_node, scratch_path, probe_file)) {
        throw std::runtime_error(fmt::format(
            "Sync verification failed: {} not found on compute node after sync", probe_file));
    }

    // Update sync baseline only after verified success
    state.last_sync_node = compute_node;
    state.last_sync_scratch = scratch_path;
    state.last_sync_manifest = local_manifest;

    cleanup_stale_scratches(compute_node, scratch_path, active_scratches);
}
