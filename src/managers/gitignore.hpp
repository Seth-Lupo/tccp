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

    // Check if a directory name should be pruned (not descended into)
    bool is_dir_ignored(const std::string& rel_dir) const;

    // Collect all non-ignored files recursively
    std::vector<fs::path> collect_files() const;

    // Get relative path from project dir
    fs::path get_relative_path(const fs::path& full_path) const;

private:
    fs::path project_dir_;

    struct Pattern {
        std::string raw;
        bool is_negation;
        bool is_dir;        // pattern ends with /
        bool has_glob;      // contains * or ?
        std::regex compiled; // pre-compiled regex (only if has_glob)
    };
    std::vector<Pattern> patterns_;

    void load_gitignore();
    void add_default_patterns();
    void add_pattern(const std::string& raw, bool is_negation);
    bool matches_pattern(const std::string& path, const Pattern& pat) const;
    static std::string glob_to_regex(const std::string& glob);
};
