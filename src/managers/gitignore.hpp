#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

class GitignoreParser {
public:
    explicit GitignoreParser(const fs::path& project_dir);

    // Check if a path should be ignored
    bool is_ignored(const std::string& path) const;
    bool is_ignored(const fs::path& path) const;

    // Collect all non-ignored files recursively
    std::vector<fs::path> collect_files() const;

    // Get relative path from project dir
    fs::path get_relative_path(const fs::path& full_path) const;

private:
    fs::path project_dir_;
    std::vector<std::pair<std::string, bool>> patterns_;  // (pattern, is_negation)

    void load_gitignore();
    void add_default_patterns();
    bool matches_pattern(const std::string& path, const std::string& pattern) const;
    std::string glob_to_regex(const std::string& glob) const;
};
