#pragma once

#include "types.hpp"
#include "ssh.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <regex>

class GitignoreParser {
public:
    explicit GitignoreParser(const fs::path& project_dir);

    bool is_ignored(const std::string& path) const;
    bool is_ignored(const fs::path& path) const;
    bool is_dir_ignored(const std::string& rel_dir) const;
    std::vector<fs::path> collect_files() const;
    fs::path get_relative_path(const fs::path& full_path) const;

private:
    fs::path project_dir_;

    struct Pattern {
        std::string raw;
        bool is_negation;
        bool is_dir;
        bool has_glob;
        std::regex compiled;
    };
    std::vector<Pattern> patterns_;

    void load_gitignore();
    void add_default_patterns();
    void add_pattern(const std::string& raw, bool is_negation);
    bool matches_pattern(const std::string& path, const Pattern& pat) const;
    static std::string glob_to_regex(const std::string& glob);
};

class Sync {
public:
    Sync(SSH& ssh, const Config& cfg);

    Result<void> push(const std::string& node, const std::string& scratch,
                      SessionState& state, StatusCallback cb = {});
    Result<void> pull_output(StatusCallback cb = {});
    Result<void> refresh(const std::string& node, const std::string& scratch,
                         SessionState& state, StatusCallback cb = {});

private:
    SSH& ssh_;
    const Config& cfg_;

    std::vector<ManifestEntry> build_manifest();
    static void diff_manifests(const std::vector<ManifestEntry>& cur,
                               const std::vector<ManifestEntry>& prev,
                               std::vector<std::string>& changed,
                               std::vector<std::string>& deleted);
};
