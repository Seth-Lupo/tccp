#pragma once

#include <string>
#include <stdexcept>

struct EnvironmentConfig {
    std::string type_name;      // "python", "python-pytorch"
    std::string docker_uri;     // "docker://python:3.11-slim"
    std::string sif_filename;   // "python_3.11-slim.sif"
    bool gpu;                   // needs --nv and --system-site-packages
};

// Returns the environment config for a given project type.
// Throws std::runtime_error if type is unknown.
const EnvironmentConfig& get_environment(const std::string& type);
