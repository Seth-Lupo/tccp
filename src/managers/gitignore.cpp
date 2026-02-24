#include "gitignore.hpp"
#include <core/utils.hpp>
#include <fstream>
#include <algorithm>

GitignoreParser::GitignoreParser(const fs::path& project_dir)
    : project_dir_(project_dir) {
    add_default_patterns();
    load_gitignore();
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
            pat.has_glob = false; // fallback to string matching
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
        // tccp internal artifacts — never upload these
        "output/",
        "tccp_run.sh",
    };

    for (const auto& pattern : defaults) {
        add_pattern(pattern, false);
    }
}

void GitignoreParser::load_gitignore() {
    // .tccpignore takes priority over .gitignore
    fs::path gitignore_path = project_dir_ / ".tccpignore";
    if (!fs::exists(gitignore_path)) {
        gitignore_path = project_dir_ / ".gitignore";
    }

    if (!fs::exists(gitignore_path)) {
        return;
    }

    std::ifstream file(gitignore_path);
    std::string line;

    while (std::getline(file, line)) {
        trim(line);

        if (line.empty() || line[0] == '#') {
            continue;
        }

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
    // Check if a directory component matches any ignore pattern.
    // Used for pruning: skip descending into ignored directories entirely.
    bool ignored = false;
    for (const auto& pat : patterns_) {
        if (pat.is_negation) {
            if (matches_pattern(rel_dir, pat)) ignored = false;
            continue;
        }
        // For dir patterns like "build/", check if dir name matches
        if (pat.is_dir) {
            std::string pat_name = pat.raw.substr(0, pat.raw.size() - 1);
            if (rel_dir == pat_name || rel_dir.find(pat_name + "/") == 0 ||
                rel_dir.find("/" + pat_name) != std::string::npos) {
                ignored = true;
            }
        }
        // Also check non-dir patterns that match directory names
        if (!pat.is_dir && !pat.has_glob) {
            if (rel_dir == pat.raw || rel_dir.find("/" + pat.raw) != std::string::npos) {
                ignored = true;
            }
        }
    }
    return ignored;
}

fs::path GitignoreParser::get_relative_path(const fs::path& full_path) const {
    if (full_path.is_relative()) {
        return full_path;
    }
    return fs::relative(full_path, project_dir_);
}

std::vector<fs::path> GitignoreParser::collect_files() const {
    std::vector<fs::path> files;

    if (!fs::exists(project_dir_)) {
        return files;
    }

    // Use non-recursive iterator + manual stack to prune ignored directories
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

    if (pattern == "*") {
        return true;
    }

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
