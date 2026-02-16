#include "gitignore.hpp"
#include <core/utils.hpp>
#include <fstream>
#include <algorithm>

GitignoreParser::GitignoreParser(const fs::path& project_dir)
    : project_dir_(project_dir) {
    add_default_patterns();
    load_gitignore();
}

void GitignoreParser::add_default_patterns() {
    // Common patterns to always ignore
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
        patterns_.push_back({pattern, false});
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

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Handle negation
        bool is_negation = false;
        if (line[0] == '!') {
            is_negation = true;
            line = line.substr(1);
        }

        patterns_.push_back({line, is_negation});
    }
}

bool GitignoreParser::is_ignored(const std::string& path) const {
    return is_ignored(fs::path(path));
}

bool GitignoreParser::is_ignored(const fs::path& path) const {
    std::string rel_path = get_relative_path(path).string();

    // Normalize path separators for pattern matching
    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');

    bool ignored = false;

    // Process patterns in order (later patterns override earlier ones)
    for (const auto& [pattern, is_negation] : patterns_) {
        if (matches_pattern(rel_path, pattern)) {
            ignored = !is_negation;
        }
    }

    return ignored;
}

fs::path GitignoreParser::get_relative_path(const fs::path& full_path) const {
    if (full_path.is_relative()) {
        return full_path;
    }

    auto rel = fs::relative(full_path, project_dir_);
    return rel;
}

std::vector<fs::path> GitignoreParser::collect_files() const {
    std::vector<fs::path> files;

    if (!fs::exists(project_dir_)) {
        return files;
    }

    for (const auto& entry : fs::recursive_directory_iterator(project_dir_)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (is_ignored(entry.path())) {
            continue;
        }

        files.push_back(entry.path());
    }

    // Sort for consistent ordering
    std::sort(files.begin(), files.end());

    return files;
}

bool GitignoreParser::matches_pattern(const std::string& path, const std::string& pattern) const {
    // Handle directory patterns (ending with /)
    bool is_dir_pattern = pattern.back() == '/';

    // Simple fnmatch-like matching
    // This is a basic implementation; production would use full gitignore semantics

    if (pattern == "*") {
        return true;
    }

    if (pattern.find('*') != std::string::npos) {
        // Convert glob to regex
        std::string regex_pattern = glob_to_regex(pattern);
        try {
            std::regex re(regex_pattern);
            return std::regex_search(path, re);
        } catch (...) {
            return false;
        }
    }

    // Exact match or prefix match
    if (is_dir_pattern) {
        std::string pattern_without_slash = pattern.substr(0, pattern.length() - 1);
        return path == pattern_without_slash || path.find(pattern_without_slash + "/") == 0;
    }

    return path == pattern || path.find("/" + pattern) != std::string::npos;
}

std::string GitignoreParser::glob_to_regex(const std::string& glob) const {
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
            // Check for **
            if (i + 1 < glob.length() && glob[i + 1] == '*') {
                regex += ".*";
                i++; // Skip next *
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
