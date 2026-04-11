#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <termios.h>

// ── Helpers ───────────────────────────────────────────────

// "4h" → "4:00:00", "30m" → "0:30:00", "1d" → "24:00:00", passthrough "HH:MM:SS"
std::string parse_time(const std::string& input) {
    if (input.empty()) return "4:00:00";
    // Already in HH:MM:SS or H:MM:SS format
    if (input.find(':') != std::string::npos) return input;

    char suffix = input.back();
    std::string num = input.substr(0, input.size() - 1);
    int val = 0;
    try { val = std::stoi(num); } catch (...) { return "4:00:00"; }

    if (suffix == 'h' || suffix == 'H')
        return fmt::format("{}:00:00", val);
    if (suffix == 'm' || suffix == 'M')
        return fmt::format("{}:{:02d}:00", val / 60, val % 60);
    if (suffix == 'd' || suffix == 'D')
        return fmt::format("{}:00:00", val * 24);

    // Bare number = hours
    try {
        val = std::stoi(input);
        return fmt::format("{}:00:00", val);
    } catch (...) {}

    return "4:00:00";
}

// "pytorch/pytorch:2.6.0-cuda12.4-cudnn9-runtime" → "pytorch_pytorch_2.6.0-cuda12.4-cudnn9-runtime.sif"
std::string sif_name(const std::string& container) {
    std::string name = container;
    // Strip docker:// prefix if present
    if (name.find("docker://") == 0) name = name.substr(9);
    // Replace / and : with _
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), ':', '_');
    return name + ".sif";
}

// Prepend docker:// if absent
std::string docker_uri(const std::string& container) {
    if (container.find("://") != std::string::npos) return container;
    return "docker://" + container;
}

// ── Path helpers ──────────────────────────────────────────

static fs::path global_config_dir() {
    return home_dir() / ".tccp";
}

static fs::path global_config_path() {
    return global_config_dir() / "config.yaml";
}

// ── Config loading ────────────────────────────────────────

static GlobalConfig load_global_config() {
    GlobalConfig g;
    fs::path path = global_config_path();
    if (!fs::exists(path)) return g;

    try {
        YAML::Node root = YAML::LoadFile(path.string());
        g.host = root["host"].as<std::string>("xfer.cluster.tufts.edu");
        g.user = root["user"].as<std::string>("");
        g.password = root["password"].as<std::string>("");
        g.partition = root["partition"].as<std::string>("gpu");
        g.gpu = root["gpu"].as<std::string>("");
        if (root["gpu-count"]) g.gpu_count = root["gpu-count"].as<int>(1);
        if (root["cpus"]) g.cpus = root["cpus"].as<int>(4);
        if (root["memory"]) g.memory = root["memory"].as<std::string>("32G");
        if (root["time"]) g.time = root["time"].as<std::string>("4h");
        if (root["cache-containers"]) g.cache_containers = root["cache-containers"].as<bool>(false);
    } catch (...) {
        // Corrupt config — use defaults
    }
    return g;
}

static ProjectConfig load_project_config(const fs::path& project_dir) {
    ProjectConfig p;
    fs::path path = project_dir / "tccp.yaml";
    if (!fs::exists(path)) return p;

    try {
        YAML::Node root = YAML::LoadFile(path.string());
        if (root["host"]) p.host = root["host"].as<std::string>("");
        p.container = root["container"].as<std::string>("");
        p.init = root["init"].as<std::string>("");

        if (root["partition"]) p.partition = root["partition"].as<std::string>("");
        if (root["gpu"]) p.gpu = root["gpu"].as<std::string>("");
        if (root["gpu-count"]) p.gpu_count = root["gpu-count"].as<int>(1);
        if (root["cpus"]) p.cpus = root["cpus"].as<int>(4);
        if (root["memory"]) p.memory = root["memory"].as<std::string>("32G");
        if (root["time"]) p.time = root["time"].as<std::string>("4h");
        if (root["output"]) p.output = root["output"].as<std::string>("output/");

        if (root["ports"]) {
            if (root["ports"].IsSequence()) {
                for (const auto& port : root["ports"])
                    p.ports.push_back(port.as<int>());
            } else if (root["ports"].IsScalar()) {
                p.ports.push_back(root["ports"].as<int>());
            }
        }

        if (root["rodata"]) {
            if (root["rodata"].IsSequence()) {
                for (const auto& r : root["rodata"])
                    p.rodata.push_back(r.as<std::string>());
            } else if (root["rodata"].IsScalar()) {
                p.rodata.push_back(root["rodata"].as<std::string>());
            }
        }
    } catch (...) {
        // Corrupt project config — use defaults
    }
    return p;
}

Result<GlobalConfig> load_config_global_only() {
    auto g = load_global_config();
    if (g.user.empty()) {
        return Result<GlobalConfig>::Err("Missing user. Run 'tccp setup' to configure.");
    }
    return Result<GlobalConfig>::Ok(std::move(g));
}

Result<Config> load_config(const fs::path& project_dir) {
    Config cfg;
    cfg.project_dir = project_dir;
    cfg.project_name = project_dir.filename().string();
    cfg.global = load_global_config();
    cfg.project = load_project_config(project_dir);

    // Project tccp.yaml overrides global
    if (!cfg.project.host.empty())
        cfg.global.host = cfg.project.host;
    if (!cfg.project.partition.empty())
        cfg.global.partition = cfg.project.partition;

    // Project overrides global for resources
    if (cfg.project.gpu.empty() && !cfg.global.gpu.empty())
        cfg.project.gpu = cfg.global.gpu;
    if (cfg.project.memory == "32G" && cfg.global.memory != "32G")
        cfg.project.memory = cfg.global.memory;
    if (cfg.project.cpus == 4 && cfg.global.cpus != 4)
        cfg.project.cpus = cfg.global.cpus;
    if (cfg.project.time == "4h" && cfg.global.time != "4h")
        cfg.project.time = cfg.global.time;

    // Validate required fields
    if (cfg.global.user.empty()) {
        return Result<Config>::Err(
            "Missing user. Run 'tccp setup' to configure.");
    }

    if (cfg.project.container.empty()) {
        return Result<Config>::Err(
            "No container specified in tccp.yaml");
    }

    return Result<Config>::Ok(std::move(cfg));
}

// ── Setup ─────────────────────────────────────────────────

Result<void> run_setup() {
    fs::path dir = global_config_dir();
    fs::create_directories(dir);

    fs::path path = global_config_path();

    std::string user, password;

    std::cout << "tccp setup\n\n";

    std::cout << "Username: ";
    std::getline(std::cin, user);
    if (user.empty()) {
        return Result<void>::Err("Username is required");
    }

    std::cout << "Password: ";
    {
        struct termios old_t, new_t;
        tcgetattr(STDIN_FILENO, &old_t);
        new_t = old_t;
        new_t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
        std::getline(std::cin, password);
        tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
        std::cout << "\n";
    }
    if (password.empty()) {
        return Result<void>::Err("Password is required");
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "user" << YAML::Value << user;
    out << YAML::Key << "password" << YAML::Value << password;
    out << YAML::EndMap;

    std::ofstream fout(path.string());
    if (!fout) {
        return Result<void>::Err("Failed to write " + path.string());
    }
    fout << out.c_str();
    fout.close();

    std::cout << "\nConfig written to " << path.string() << "\n";
    return Result<void>::Ok();
}
