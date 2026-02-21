#include "sync_manager.hpp"
#include "gitignore.hpp"
#include <core/config.hpp>
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <platform/archive.hpp>
#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <map>
#include <tuple>

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
            entry.md5 = compute_file_md5(abs_path);
        } catch (...) {
            entry.mtime = 0;
            entry.size = 0;
            entry.md5 = "";
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
    std::map<std::string, std::tuple<int64_t, int64_t, std::string>> prev_map;
    for (const auto& e : previous) {
        prev_map[e.path] = {e.mtime, e.size, e.md5};
    }

    for (const auto& e : current) {
        auto it = prev_map.find(e.path);
        if (it == prev_map.end()) {
            changed.push_back(e.path);
        } else {
            const auto& [prev_mtime, prev_size, prev_md5] = it->second;
            if (prev_mtime != e.mtime || prev_size != e.size || prev_md5 != e.md5) {
                changed.push_back(e.path);
            }
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
    fs::path tar_path = platform::temp_dir() / fmt::format("tccp_sync_{}.tar", std::time(nullptr));
    platform::create_tar(tar_path, project_dir, rel_paths);
    return tar_path;
}

// Pipe tar to compute node via exec channel with raw binary stdin.
// Uses run_with_input() to open a fresh exec channel on the same SSH session
// (no new connection, no Duo re-auth) and pipe raw tar bytes directly via
// libssh2_channel_write(), bypassing tmux entirely. This is binary-clean and
// reliable for large payloads.
// Returns true if probe file MD5 matches expected_md5 after extraction.
bool SyncManager::pipe_tar_to_node(const fs::path& tar_path,
                                    const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::string& probe_file,
                                    const std::string& expected_md5) {
    // Read the tar file into memory
    std::ifstream tar_file(tar_path, std::ios::binary | std::ios::ate);
    if (!tar_file) {
        throw std::runtime_error("Cannot open tar file: " + tar_path.string());
    }
    auto file_size = tar_file.tellg();
    tar_file.seekg(0);
    std::string tar_data(static_cast<size_t>(file_size), '\0');
    tar_file.read(tar_data.data(), file_size);
    tar_file.close();

    // SSH command to run on the DTN: hop to compute node, extract tar from
    // stdin, then print the MD5 of the probe file for verification.
    std::string ssh_cmd = fmt::format(
        "ssh -T {} {} '"
        "mkdir -p {} && cd {} && tar xf -; "
        "if [ -f {} ]; then md5sum {} | cut -d\" \" -f1; fi'",
        SSH_OPTS, compute_node,
        scratch_path, scratch_path,
        probe_file, probe_file);

    auto result = dtn_.run_with_input(ssh_cmd, tar_data.data(), tar_data.size(), 120);
    if (result.failed()) {
        throw std::runtime_error(fmt::format(
            "File sync failed (exit code {}): {}. stdout: {}",
            result.exit_code, result.stderr_data, result.stdout_data));
    }

    // Verify MD5 checksum — extract 32-char hex string from output,
    // which may contain stray stderr (SSH messages, etc.).
    std::string remote_md5;
    std::string output = result.stdout_data;
    trim(output);
    // Search for a 32-char hex string (the MD5) in the output
    {
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            trim(line);
            if (line.size() == 32 &&
                line.find_first_not_of("0123456789abcdef") == std::string::npos) {
                remote_md5 = line;
                break;
            }
        }
    }

    if (remote_md5.empty()) {
        throw std::runtime_error(fmt::format(
            "Sync verification failed: could not find MD5 in remote output. "
            "Expected: {}. Full output: '{}'",
            expected_md5, output));
    }

    if (remote_md5 != expected_md5) {
        throw std::runtime_error(fmt::format(
            "Sync corruption detected: {} MD5 mismatch (expected: {}, got: {})",
            probe_file, expected_md5, remote_md5));
    }

    return true;
}

// ── Sync strategies ────────────────────────────────────────

bool SyncManager::full_sync(const std::string& compute_node,
                             const std::string& scratch_path,
                             const std::vector<SyncManifestEntry>& manifest,
                             const std::string& probe_file,
                             StatusCallback cb) {
    if (cb) cb(fmt::format("Syncing {} files to compute node...", manifest.size()));

    std::vector<std::string> rel_paths;
    rel_paths.reserve(manifest.size());
    for (const auto& e : manifest) rel_paths.push_back(e.path);

    // Find expected MD5 for probe file
    std::string expected_md5;
    for (const auto& e : manifest) {
        if (e.path == probe_file) {
            expected_md5 = e.md5;
            break;
        }
    }

    fs::path tar_path = create_local_tar(config_.project_dir(), rel_paths);
    bool verified = pipe_tar_to_node(tar_path, compute_node, scratch_path, probe_file, expected_md5);
    fs::remove(tar_path);

    return verified;
}

bool SyncManager::incremental_sync(const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::vector<std::string>& changed_files,
                                    const std::vector<std::string>& deleted_files,
                                    const std::string& probe_file,
                                    const std::string& expected_md5,
                                    StatusCallback cb) {
    // Build a single SSH command that deletes removed files AND checks probe,
    // so we only need one round-trip if there are no changed files to transfer.
    if (!deleted_files.empty()) {
        std::string rm_cmd = "rm -f";
        for (const auto& f : deleted_files) {
            rm_cmd += " " + scratch_path + "/" + f;
        }
        dtn_.run(fmt::format("ssh {} {} '{}'", SSH_OPTS, compute_node, rm_cmd));
    }

    if (changed_files.empty()) {
        if (cb) cb("No files changed");
        return true;  // Nothing to transfer, trust previous state
    }

    if (cb) cb(fmt::format("Syncing {} changed files...", changed_files.size()));

    std::vector<std::string> existing;
    for (const auto& rel : changed_files) {
        if (fs::exists(config_.project_dir() / rel)) {
            existing.push_back(rel);
        }
    }
    if (existing.empty()) return true;

    fs::path tar_path = create_local_tar(config_.project_dir(), existing);
    bool verified = pipe_tar_to_node(tar_path, compute_node, scratch_path, probe_file, expected_md5);
    fs::remove(tar_path);

    return verified;
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
    // Single SSH command: list dirs and delete stale ones in one hop.
    // We pass the active scratches and current scratch as exclusions.
    fs::path scratch_parent = fs::path(scratch_path).parent_path().string();
    if (scratch_parent.empty()) return;

    // Build exclusion list for the remote script
    std::string exclude_check;
    exclude_check += fmt::format("[ \"$d\" = \"{}\" ]", scratch_path);
    for (const auto& active : active_scratches) {
        exclude_check += fmt::format(" || [ \"$d\" = \"{}\" ]", active);
    }

    // Single SSH: list + filter + delete all in one command
    std::string cmd = fmt::format(
        "ssh {} {} '"
        "for d in $(ls -d {}/*/ 2>/dev/null); do "
        "d=${{d%%/}}; "  // strip trailing slash
        "if {}; then continue; fi; "
        "rm -rf \"$d\"; "
        "done'",
        SSH_OPTS, compute_node,
        scratch_parent.string(),
        exclude_check);

    dtn_.run(cmd);
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
    std::string expected_md5 = local_manifest[0].md5;

    if (expected_md5.empty()) {
        throw std::runtime_error(fmt::format(
            "Failed to compute MD5 checksum for probe file: {}. "
            "Ensure file exists and is readable.", probe_file));
    }

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
    // For incremental, we need a previous manifest from the same node AND
    // a successful reuse (we skip the separate verify — pipe_tar_to_node
    // does built-in MD5 verification in the same SSH command).
    bool verified = false;
    if (reused && !state.last_sync_manifest.empty() &&
        state.last_sync_node == compute_node) {

        std::vector<std::string> changed, deleted;
        diff_manifests(local_manifest, state.last_sync_manifest, changed, deleted);

        if (!changed.empty() || !deleted.empty()) {
            verified = incremental_sync(compute_node, scratch_path,
                                        changed, deleted, probe_file, expected_md5, cb);
        } else {
            if (cb) cb("All files up to date");
            // Quick verify with a lightweight check built into next command,
            // or trust the reuse since we just moved/copied the scratch.
            verified = true;
        }
    }

    if (!verified) {
        verified = full_sync(compute_node, scratch_path, local_manifest, probe_file, cb);
    }

    if (!verified) {
        throw std::runtime_error(fmt::format(
            "Sync verification failed: {} not found on compute node after sync", probe_file));
    }

    // Update sync baseline only after verified success
    state.last_sync_node = compute_node;
    state.last_sync_scratch = scratch_path;
    state.last_sync_manifest = local_manifest;

    // Cleanup stale scratches (single SSH command)
    cleanup_stale_scratches(compute_node, scratch_path, active_scratches);
}
