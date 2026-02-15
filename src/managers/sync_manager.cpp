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

std::vector<SyncManifestEntry> SyncManager::get_remote_manifest(
    const std::string& compute_node,
    const std::string& scratch_path) {

    std::vector<SyncManifestEntry> manifest;

    // One SSH command: get all file paths + sizes from scratch
    auto result = dtn_.run(fmt::format(
        "ssh {} {} 'cd {} && find . -type f -printf \"%s %P\\n\" 2>/dev/null'",
        SSH_OPTS, compute_node, scratch_path));

    if (result.exit_code != 0 || result.stdout_data.empty()) {
        return manifest;  // empty → caller falls back to full sync
    }

    // Parse "size path" lines
    std::istringstream stream(result.stdout_data);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        auto space_pos = line.find(' ');
        if (space_pos == std::string::npos) continue;

        SyncManifestEntry entry;
        try {
            entry.size = std::stoll(line.substr(0, space_pos));
        } catch (...) {
            continue;
        }
        entry.path = line.substr(space_pos + 1);
        entry.mtime = 0;  // remote mtime not comparable to local
        manifest.push_back(entry);
    }

    return manifest;
}

std::vector<std::string> SyncManager::diff_manifests(
    const std::vector<SyncManifestEntry>& local,
    const std::vector<SyncManifestEntry>& remote) {

    // Build remote lookup: path → size
    std::map<std::string, int64_t> remote_map;
    for (const auto& e : remote) {
        remote_map[e.path] = e.size;
    }

    // Find files that are new or changed (size differs)
    std::vector<std::string> changed;
    for (const auto& e : local) {
        auto it = remote_map.find(e.path);
        if (it == remote_map.end()) {
            changed.push_back(e.path);
        } else if (it->second != e.size) {
            changed.push_back(e.path);
        }
    }

    return changed;
}

// Collect all files that should be synced (project files + rodata + env file).
// Returns a list of (absolute_path, relative_path) pairs.
std::vector<std::pair<fs::path, std::string>> SyncManager::collect_sync_files() {
    std::vector<std::pair<fs::path, std::string>> result;
    std::set<std::string> seen;

    const auto& proj = config_.project();

    // Project files (respecting .gitignore)
    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();
    for (const auto& f : files) {
        auto rel = parser.get_relative_path(f).string();
        if (seen.insert(rel).second) {
            result.emplace_back(f, rel);
        }
    }

    // Rodata files
    for (const auto& rodata_path : proj.rodata) {
        fs::path src = rodata_path;
        if (src.is_relative()) src = config_.project_dir() / src;
        if (!fs::exists(src)) continue;

        if (fs::is_directory(src)) {
            for (const auto& entry : fs::recursive_directory_iterator(src)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), config_.project_dir()).string();
                if (seen.insert(rel).second) {
                    result.emplace_back(entry.path(), rel);
                }
            }
        }
    }

    // Env file (bypasses gitignore)
    if (!proj.env_file.empty()) {
        fs::path env_path = proj.env_file;
        if (env_path.is_relative()) env_path = config_.project_dir() / env_path;
        if (fs::exists(env_path) && fs::is_regular_file(env_path)) {
            auto rel = fs::relative(env_path, config_.project_dir()).string();
            if (seen.insert(rel).second) {
                result.emplace_back(env_path, rel);
            }
        }
    }

    return result;
}

// Create a tar archive from a list of (absolute_path, relative_path) pairs.
// Returns the path to the temp tar file.
static fs::path create_local_tar(const fs::path& project_dir,
                                  const std::vector<std::pair<fs::path, std::string>>& files) {
    // Write file list to a temp manifest (tar -T reads from it)
    fs::path tar_path = fs::temp_directory_path() / fmt::format("tccp_sync_{}.tar", std::time(nullptr));
    fs::path list_path = fs::temp_directory_path() / fmt::format("tccp_sync_{}.list", std::time(nullptr));

    {
        std::ofstream list(list_path);
        for (const auto& [abs, rel] : files) {
            list << rel << "\n";
        }
    }

    // Create tar from project directory using the file list
    std::string cmd = fmt::format("tar cf {} -C {} -T {}",
                                  tar_path.string(), project_dir.string(), list_path.string());
    std::system(cmd.c_str());

    fs::remove(list_path);
    return tar_path;
}

// Pipe a local tar archive through DTN directly to compute node (no DTN disk usage).
// The tar data is base64-encoded and sent as a heredoc that decodes and pipes to ssh.
void SyncManager::pipe_tar_to_node(const fs::path& tar_path,
                                    const std::string& compute_node,
                                    const std::string& scratch_path) {
    // Read the tar file
    std::ifstream file(tar_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Base64-encode for binary safety over PTY
    // (reuse the connection's upload-style heredoc approach)
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto b64_encode = [](const std::string& input) -> std::string {
        std::string out;
        out.reserve(((input.size() + 2) / 3) * 4);
        const auto* data = reinterpret_cast<const unsigned char*>(input.data());
        size_t len = input.size();
        for (size_t i = 0; i < len; i += 3) {
            unsigned val = data[i] << 16;
            if (i + 1 < len) val |= data[i + 1] << 8;
            if (i + 2 < len) val |= data[i + 2];
            out += B64[(val >> 18) & 0x3F];
            out += B64[(val >> 12) & 0x3F];
            out += (i + 1 < len) ? B64[(val >> 6) & 0x3F] : '=';
            out += (i + 2 < len) ? B64[val & 0x3F] : '=';
        }
        return out;
    };

    std::string encoded = b64_encode(content);

    // Split into 76-char lines (standard base64 line length)
    std::string lines;
    lines.reserve(encoded.size() + encoded.size() / 76 + 1);
    for (size_t i = 0; i < encoded.size(); i += 76) {
        lines.append(encoded, i, std::min<size_t>(76, encoded.size() - i));
        lines += '\n';
    }

    // Pipe through DTN: base64 decode → ssh to compute node → untar
    // No files touch DTN disk — the data flows directly through the pipe.
    std::string cmd = fmt::format(
        "base64 -d << 'TCCP_B64_EOF' | ssh {} {} 'mkdir -p {} && cd {} && tar xf -'\n{}TCCP_B64_EOF",
        SSH_OPTS, compute_node, scratch_path, scratch_path, lines);

    dtn_.run(cmd, 600);  // 10 min timeout for large syncs
}

void SyncManager::full_sync(const std::string& compute_node,
                             const std::string& scratch_path,
                             StatusCallback cb) {
    auto all_files = collect_sync_files();

    if (cb) cb(fmt::format("Syncing {} files to compute node...", all_files.size()));

    // Create tar locally
    fs::path tar_path = create_local_tar(config_.project_dir(), all_files);

    // Pipe through DTN directly to compute node (no DTN disk usage)
    pipe_tar_to_node(tar_path, compute_node, scratch_path);

    // Clean up local temp
    fs::remove(tar_path);

    if (cb) cb("Full sync complete");
}

void SyncManager::incremental_sync(const std::string& compute_node,
                                    const std::string& scratch_path,
                                    const std::vector<std::string>& changed_files,
                                    const std::vector<std::string>& deleted_files,
                                    StatusCallback cb) {
    // Delete removed files on remote
    if (!deleted_files.empty()) {
        // Batch deletes into a single SSH command
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

    // Build file list for tar (only files that exist locally)
    std::vector<std::pair<fs::path, std::string>> files_to_sync;
    for (const auto& rel_path : changed_files) {
        fs::path local_file = config_.project_dir() / rel_path;
        if (fs::exists(local_file)) {
            files_to_sync.emplace_back(local_file, rel_path);
        }
    }

    if (files_to_sync.empty()) return;

    // Create local tar of changed files and pipe through DTN to compute node
    fs::path tar_path = create_local_tar(config_.project_dir(), files_to_sync);
    pipe_tar_to_node(tar_path, compute_node, scratch_path);
    fs::remove(tar_path);

    if (cb) cb(fmt::format("Incremental sync: {} files updated", changed_files.size()));
}

void SyncManager::sync_to_scratch(const std::string& compute_node,
                                   const std::string& scratch_path,
                                   ProjectState& state,
                                   StatusCallback cb) {

    // Build current local manifest (local is always source of truth)
    auto local_manifest = build_local_manifest();

    // Runtime artifact prefixes/suffixes to skip when computing deletes.
    // These are created by the job at runtime, not synced from local.
    static const std::vector<std::string> skip_prefixes = {"cache/", "output/", "env/"};
    static const std::vector<std::string> skip_suffixes = {".sock", ".log"};
    static const std::vector<std::string> skip_exact = {"tccp_run.sh"};

    auto is_runtime_artifact = [&](const std::string& path) {
        for (const auto& p : skip_prefixes)
            if (path.substr(0, p.size()) == p) return true;
        for (const auto& s : skip_suffixes)
            if (path.size() >= s.size() && path.substr(path.size() - s.size()) == s) return true;
        for (const auto& e : skip_exact)
            if (path == e) return true;
        return false;
    };

    // Check if we can reuse files from a previous scratch on the same node
    bool tried_cp = false;
    bool cp_ok = false;

    if (!state.last_sync_node.empty() &&
        state.last_sync_node == compute_node &&
        !state.last_sync_scratch.empty() &&
        state.last_sync_scratch != scratch_path) {

        // Old scratch on same node — copy only project files (skip runtime
        // artifacts like cache/, output/, env/ which can be many GBs).
        // Uses tar with excludes piped locally on the compute node (no network).
        if (cb) cb("Copying files from previous scratch...");
        auto cp_result = dtn_.run(fmt::format(
            "ssh {} {} '"
            "if [ -d {} ]; then "
            "mkdir -p {} && "
            "cd {} && "
            "tar cf - "
            "--exclude=cache --exclude=output --exclude=env "
            "--exclude=.tccp-tmp --exclude=\"*.sock\" --exclude=\"*.log\" "
            "--exclude=tccp_run.sh . 2>/dev/null | "
            "(cd {} && tar xf -) && "
            "echo CP_OK; "
            "else echo CP_NONE; fi'",
            SSH_OPTS, compute_node,
            state.last_sync_scratch,
            scratch_path,
            state.last_sync_scratch,
            scratch_path));
        tried_cp = true;
        cp_ok = cp_result.stdout_data.find("CP_OK") != std::string::npos;
    }

    if (cp_ok) {
        // Get actual remote file listing and diff against local
        if (cb) cb("Verifying remote files...");
        auto remote_manifest = get_remote_manifest(compute_node, scratch_path);

        if (remote_manifest.empty()) {
            // Remote listing failed — fall back to full sync
            full_sync(compute_node, scratch_path, cb);
        } else {
            auto changed = diff_manifests(local_manifest, remote_manifest);

            // Find files present remotely but removed locally (skip runtime artifacts)
            std::set<std::string> local_set;
            for (const auto& e : local_manifest) local_set.insert(e.path);
            std::vector<std::string> deleted;
            for (const auto& e : remote_manifest) {
                if (local_set.find(e.path) == local_set.end() && !is_runtime_artifact(e.path)) {
                    deleted.push_back(e.path);
                }
            }

            if (!changed.empty() || !deleted.empty()) {
                incremental_sync(compute_node, scratch_path, changed, deleted, cb);
            } else {
                if (cb) cb("All files up to date");
            }
        }

        // Clean up old scratch
        dtn_.run(fmt::format("ssh {} {} 'rm -rf {}'",
                             SSH_OPTS, compute_node, state.last_sync_scratch));
    } else {
        // No reusable scratch — full sync from local
        full_sync(compute_node, scratch_path, cb);
    }

    // Update state (no manifest stored — we always verify against remote)
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
