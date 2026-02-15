#include "environment.hpp"
#include <unordered_map>

static const std::unordered_map<std::string, EnvironmentConfig>& registry() {
    static const std::unordered_map<std::string, EnvironmentConfig> configs = {
        {"python", {
            "python",
            "docker://python:3.11-slim",
            "python_3.11-slim.sif",
            false,
        }},
        {"python-pytorch", {
            "python-pytorch",
            "docker://pytorch/pytorch:2.6.0-cuda12.4-cudnn9-runtime",
            "pytorch_2.6.0-cuda12.4-runtime.sif",
            true,
        }},
    };
    return configs;
}

const EnvironmentConfig& get_environment(const std::string& type) {
    auto& configs = registry();
    auto it = configs.find(type);
    if (it == configs.end()) {
        throw std::runtime_error("Unknown environment type: " + type);
    }
    return it->second;
}
