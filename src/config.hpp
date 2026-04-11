#pragma once

#include "types.hpp"
#include <filesystem>

Result<Config> load_config(const fs::path& project_dir = fs::current_path());
Result<GlobalConfig> load_config_global_only();
Result<void> run_setup();

// Internal helpers (exposed for testing)
std::string parse_time(const std::string& input);
std::string sif_name(const std::string& container);
std::string docker_uri(const std::string& container);
