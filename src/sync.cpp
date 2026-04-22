#include "sync.hpp"
#include <fmt/format.h>
#include <fstream>
#include <algorithm>
#include <set>
#include <map>

// ── GitignoreParser ───────────────────────────────────────

GitignoreParser::GitignoreParser(const fs::path& project_dir,
                                 std::vector<std::string> force_includes)
    : project_dir_(project_dir), force_includes_(std::move(force_includes)) {
    for (auto& fi : force_includes_) {
        while (!fi.empty() && fi.back() == '/') fi.pop_back();
        std::replace(fi.begin(), fi.end(), '\\', '/');
    }
    add_default_patterns();
    load_gitignore();
}

bool GitignoreParser::is_force_included(const std::string& rel_path) const {
    for (const auto& fi : force_includes_) {
        if (fi.empty()) continue;
        if (rel_path == fi) return true;
        if (rel_path.size() > fi.size() && rel_path.compare(0, fi.size(), fi) == 0
            && rel_path[fi.size()] == '/') return true;
    }
    return false;
}

void GitignoreParser::add_pattern(const std::string& raw, bool is_negation) {
    Pattern pat;
    pat.raw = raw;
    pat.is_negation = is_negation;
    pat.is_dir = !raw.empty() && raw.back() == '/';
    pat.has_glob = raw.find('*') != std::string::npos || raw.find('?') != std::string::npos;
    if (pat.has_glob) {
        try {
            pat.compiled = std::regex(glob_to_regex(raw));
        } catch (...) {
            pat.has_glob = false;
        }
    }
    patterns_.push_back(std::move(pat));
}

void GitignoreParser::add_default_patterns() {
    std::vector<std::string> defaults = {
        ".git/",
        ".gitignore",
        ".tccpignore",
        "__pycache__/",
        "*.pyc",
        "*.pyo",
        ".venv/",
        "venv/",
        ".idea/",
        ".vscode/",
        ".claude/",
        ".DS_Store",
        "*.swp",
        "*.swo",
        "*~",
        ".cache/",
        "build/",
        "dist/",
        "*.egg-info/",
        ".pytest_cache/",
        ".mypy_cache/",
        "node_modules/",
        ".env",
        "output/",
        ".tccp.sock",
        ".tccp-env.sh",
    };

    for (const auto& pattern : defaults) {
        add_pattern(pattern, false);
    }
}

void GitignoreParser::load_gitignore() {
    fs::path gitignore_path = project_dir_ / ".tccpignore";
    if (!fs::exists(gitignore_path)) {
        gitignore_path = project_dir_ / ".gitignore";
    }
    if (!fs::exists(gitignore_path)) return;

    std::ifstream file(gitignore_path);
    std::string line;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        bool is_negation = false;
        if (line[0] == '!') {
            is_negation = true;
            line = line.substr(1);
        }
        add_pattern(line, is_negation);
    }
}

bool GitignoreParser::is_ignored(const std::string& path) const {
    return is_ignored(fs::path(path));
}

bool GitignoreParser::is_ignored(const fs::path& path) const {
    {
        auto rel = get_relative_path(path).generic_string();
        if (is_force_included(rel)) return false;
    }
    std::string rel_path = get_relative_path(path).string();
    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');

    bool ignored = false;
    for (const auto& pat : patterns_) {
        if (matches_pattern(rel_path, pat)) {
            ignored = !pat.is_negation;
        }
    }
    return ignored;
}

bool GitignoreParser::is_dir_ignored(const std::string& rel_dir) const {
    if (is_force_included(rel_dir)) return false;
    for (const auto& fi : force_includes_) {
        if (fi.size() > rel_dir.size() && fi.compare(0, rel_dir.size(), rel_dir) == 0
            && fi[rel_dir.size()] == '/') return false;
    }
    bool ignored = false;
    for (const auto& pat : patterns_) {
        if (pat.is_negation) {
            if (matches_pattern(rel_dir, pat)) ignored = false;
            continue;
        }
        if (pat.is_dir) {
            std::string pat_name = pat.raw.substr(0, pat.raw.size() - 1);
            if (rel_dir == pat_name || rel_dir.find(pat_name + "/") == 0 ||
                rel_dir.find("/" + pat_name) != std::string::npos) {
                ignored = true;
            }
        }
        if (!pat.is_dir && !pat.has_glob) {
            if (rel_dir == pat.raw || rel_dir.find("/" + pat.raw) != std::string::npos) {
                ignored = true;
            }
        }
    }
    return ignored;
}

fs::path GitignoreParser::get_relative_path(const fs::path& full_path) const {
    if (full_path.is_relative()) return full_path;
    return fs::relative(full_path, project_dir_);
}

std::vector<fs::path> GitignoreParser::collect_files() const {
    std::vector<fs::path> files;
    if (!fs::exists(project_dir_)) return files;

    std::vector<fs::path> dirs_to_visit;
    dirs_to_visit.push_back(project_dir_);

    while (!dirs_to_visit.empty()) {
        fs::path dir = std::move(dirs_to_visit.back());
        dirs_to_visit.pop_back();

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                std::string rel = fs::relative(entry.path(), project_dir_).string();
                std::replace(rel.begin(), rel.end(), '\\', '/');
                if (!is_dir_ignored(rel)) {
                    dirs_to_visit.push_back(entry.path());
                }
            } else if (entry.is_regular_file()) {
                if (!is_ignored(entry.path())) {
                    files.push_back(entry.path());
                }
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

bool GitignoreParser::matches_pattern(const std::string& path, const Pattern& pat) const {
    if (pat.has_glob) {
        return std::regex_search(path, pat.compiled);
    }

    const std::string& pattern = pat.raw;
    if (pattern == "*") return true;

    if (pat.is_dir) {
        std::string pattern_without_slash = pattern.substr(0, pattern.length() - 1);
        return path == pattern_without_slash || path.find(pattern_without_slash + "/") == 0;
    }

    return path == pattern || path.find("/" + pattern) != std::string::npos;
}

std::string GitignoreParser::glob_to_regex(const std::string& glob) {
    std::string regex;
    bool escape = false;

    for (size_t i = 0; i < glob.length(); ++i) {
        char c = glob[i];
        if (escape) {
            regex += c;
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '*') {
            if (i + 1 < glob.length() && glob[i + 1] == '*') {
                regex += ".*";
                i++;
            } else {
                regex += "[^/]*";
            }
        } else if (c == '?') {
            regex += "[^/]";
        } else if (c == '.') {
            regex += "\\.";
        } else if (c == '[') {
            regex += '[';
        } else if (c == ']') {
            regex += ']';
        } else {
            regex += c;
        }
    }
    return regex;
}

// ── Sync ──────────────────────────────────────────────────

Sync::Sync(SSH& ssh, const Config& cfg) : ssh_(ssh), cfg_(cfg) {}

std::vector<ManifestEntry> Sync::build_manifest() {
    GitignoreParser parser(cfg_.project_dir, cfg_.project.rodata);
    auto files = parser.collect_files();

    std::vector<ManifestEntry> manifest;
    manifest.reserve(files.size());

    for (const auto& file : files) {
        auto rel = parser.get_relative_path(file);
        auto ftime = fs::last_write_time(file);
        auto time_since_epoch = ftime.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count();

        ManifestEntry entry;
        entry.path = rel.string();
        entry.mtime = static_cast<int64_t>(seconds);
        entry.size = static_cast<int64_t>(fs::file_size(file));
        manifest.push_back(std::move(entry));
    }
    return manifest;
}

void Sync::diff_manifests(const std::vector<ManifestEntry>& cur,
                          const std::vector<ManifestEntry>& prev,
                          std::vector<std::string>& changed,
                          std::vector<std::string>& deleted) {
    // Build lookup from previous manifest
    std::map<std::string, const ManifestEntry*> prev_map;
    for (const auto& e : prev) {
        prev_map[e.path] = &e;
    }

    // Find changed/new files
    std::set<std::string> cur_paths;
    for (const auto& e : cur) {
        cur_paths.insert(e.path);
        auto it = prev_map.find(e.path);
        if (it == prev_map.end()) {
            changed.push_back(e.path);  // new file
        } else if (it->second->mtime != e.mtime || it->second->size != e.size) {
            changed.push_back(e.path);  // modified
        }
    }

    // Find deleted files
    for (const auto& e : prev) {
        if (cur_paths.find(e.path) == cur_paths.end()) {
            deleted.push_back(e.path);
        }
    }
}

Result<void> Sync::push(const std::string& node, const std::string& scratch,
                        SessionState& state, StatusCallback cb) {
    auto manifest = build_manifest();

    std::vector<std::string> changed, deleted;
    if (state.manifest.empty()) {
        // First sync: push everything
        for (const auto& e : manifest) {
            changed.push_back(e.path);
        }
    } else {
        diff_manifests(manifest, state.manifest, changed, deleted);
    }

    if (changed.empty() && deleted.empty()) {
        if (cb) cb("No changes to sync");
        state.manifest = manifest;
        return Result<void>::Ok();
    }

    if (cb) cb(fmt::format("Syncing {} changed, {} deleted", changed.size(), deleted.size()));

    // Push changed files via tar
    if (!changed.empty()) {
        auto result = ssh_.tar_push(node, cfg_.project_dir, changed, scratch);
        if (result.is_err()) return result;
    }

    // Remove deleted files on remote
    if (!deleted.empty()) {
        std::string rm_cmd;
        for (const auto& d : deleted) {
            rm_cmd += fmt::format("rm -f {}/{} ; ", scratch, d);
        }
        ssh_.run_compute(node, rm_cmd);
    }

    state.manifest = manifest;
    if (cb) cb(fmt::format("Synced {} files", changed.size()));
    return Result<void>::Ok();
}

Result<void> Sync::pull_output(StatusCallback cb) {
    std::string output_dir = cfg_.project.output;
    // Strip trailing slash
    while (!output_dir.empty() && output_dir.back() == '/') output_dir.pop_back();
    if (output_dir.empty()) output_dir = "output";

    std::string nfs_output = fmt::format("~/.tccp/projects/{}/output", cfg_.project_name);

    // Check if remote output dir has anything
    auto check = ssh_.run(fmt::format("test -d {} && ls {} | head -1", nfs_output, nfs_output));
    if (!check.ok() || check.out.empty()) {
        if (cb) cb("No output to pull");
        return Result<void>::Ok();
    }

    if (cb) cb("Pulling output...");
    fs::path local_output = cfg_.project_dir / output_dir;
    auto result = ssh_.tar_pull(nfs_output, local_output);
    if (result.is_err()) return result;

    if (cb) cb(fmt::format("Output pulled to {}/", output_dir));
    return Result<void>::Ok();
}

Result<void> Sync::refresh(const std::string& node, const std::string& scratch,
                           SessionState& state, StatusCallback cb) {
    // Push code changes, pull output
    auto push_result = push(node, scratch, state, cb);
    if (push_result.is_err()) return push_result;
    return pull_output(cb);
}
