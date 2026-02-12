#pragma once

#include <string>
#include <optional>
#include <filesystem>
#include "types.hpp"

namespace fs = std::filesystem;

class Config {
public:
    // Load global config from ~/.tccp/config.yaml
    static Result<Config> load_global();

    // Load project config from ./tccp.yaml
    static Result<Config> load_project(const fs::path& dir = fs::current_path());

    // Load both and combine (prefer project overrides)
    static Result<Config> load(const fs::path& project_dir = fs::current_path());

    // Accessors
    const DTNConfig& dtn() const { return dtn_; }
    const LoginConfig& login() const { return login_; }
    const SlurmDefaults& slurm() const { return slurm_; }
    const ProjectConfig& project() const { return project_; }
    const fs::path& project_dir() const { return project_dir_; }

    const std::vector<std::string>& modules() const { return modules_; }
    std::optional<std::string> duo_auto() const { return duo_auto_; }

public:
    Config() = default;

private:

    DTNConfig dtn_;
    LoginConfig login_;
    SlurmDefaults slurm_;
    ProjectConfig project_;
    fs::path project_dir_;
    std::vector<std::string> modules_;
    std::optional<std::string> duo_auto_;

    friend class ConfigBuilder;
};

// Helper to check if configs exist
bool global_config_exists();
bool project_config_exists(const fs::path& dir = fs::current_path());

// Get paths
fs::path get_global_config_dir();
fs::path get_global_config_path();
fs::path get_project_config_path(const fs::path& dir = fs::current_path());

// Create default global config
Result<void> create_default_global_config();
