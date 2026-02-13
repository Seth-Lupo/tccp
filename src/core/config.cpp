#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

bool global_config_exists() {
    return fs::exists(get_global_config_path());
}

bool project_config_exists(const fs::path& dir) {
    return fs::exists(get_project_config_path(dir));
}

fs::path get_global_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return fs::path(home) / ".tccp";
}

fs::path get_global_config_path() {
    return get_global_config_dir() / "config.yaml";
}

fs::path get_project_config_path(const fs::path& dir) {
    return dir / "tccp.yaml";
}

Result<void> create_default_global_config() {
    fs::path config_path = get_global_config_path();

    // Don't overwrite existing config
    if (fs::exists(config_path)) {
        return Result<void>::Ok();
    }

    // Ensure directory exists
    fs::create_directories(config_path.parent_path());

    // Default config content
    const char* default_config = R"(# Tufts Cluster Configuration
# Edit this file to configure your cluster connection settings

dtn:
  host: "xfer.cluster.tufts.edu"
  user: ""                         # Will use credentials from keychain
  timeout: 30

login:
  host: "login.pax.tufts.edu"
  user: ""                         # Will use credentials from keychain
  email: ""                        # For job notifications
  timeout: 30

# Optional: Default SLURM settings (can be overridden per-project)
slurm:
  partition: "batch"
  time: "4:00:00"
  mem: "4G"
  cpus: 1
  gres: ""

# Optional: Environment modules to load
modules: []

# Optional: Duo auto-push setting
duo_auto: "push"
)";

    try {
        std::ofstream out(config_path);
        if (!out) {
            return Result<void>::Err("Failed to create config file at " + config_path.string());
        }
        out << default_config;
        out.close();
        return Result<void>::Ok();
    } catch (const std::exception& e) {
        return Result<void>::Err("Failed to write config file: " + std::string(e.what()));
    }
}

static DTNConfig parse_dtn_config(const YAML::Node& node) {
    DTNConfig dtn;
    dtn.host = node["host"].as<std::string>("");
    dtn.user = node["user"].as<std::string>("");
    dtn.timeout = node["timeout"].as<int>(30);

    if (node["password"]) {
        dtn.password = node["password"].as<std::string>();
    }

    if (node["ssh_key_path"]) {
        dtn.ssh_key_path = node["ssh_key_path"].as<std::string>();
    }

    return dtn;
}

static LoginConfig parse_login_config(const YAML::Node& node) {
    LoginConfig login;
    login.host = node["host"].as<std::string>("");
    login.user = node["user"].as<std::string>("");
    login.password = node["password"].as<std::string>("");
    login.email = node["email"].as<std::string>("");
    login.timeout = node["timeout"].as<int>(30);
    login.remote_dir = node["remote_dir"].as<std::string>("");

    if (node["ssh_key_path"]) {
        login.ssh_key_path = node["ssh_key_path"].as<std::string>();
    }

    return login;
}

static SlurmDefaults parse_slurm_config(const YAML::Node& node) {
    SlurmDefaults slurm;
    slurm.partition = node["partition"].as<std::string>("batch");
    slurm.time = node["time"].as<std::string>("00:30:00");
    slurm.nodes = node["nodes"].as<int>(1);
    slurm.cpus_per_task = node["cpus_per_task"].as<int>(1);
    slurm.memory = node["memory"].as<std::string>("4G");
    slurm.gpu_type = node["gpu_type"].as<std::string>("");
    slurm.gpu_count = node["gpu_count"].as<int>(0);
    slurm.mail_type = node["mail_type"].as<std::string>("NONE");

    return slurm;
}

static ProjectConfig parse_project_config(const YAML::Node& node) {
    ProjectConfig project;
    project.name = node["name"].as<std::string>("");
    project.type = node["type"].as<std::string>("python");
    project.description = node["description"].as<std::string>("");
    project.remote_dir = node["remote_dir"].as<std::string>("");

    if (node["environment"] && node["environment"].IsMap()) {
        for (const auto& kv : node["environment"]) {
            project.environment[kv.first.as<std::string>()] = kv.second.as<std::string>("");
        }
    }

    if (node["slurm"] && node["slurm"].IsMap()) {
        project.slurm = parse_slurm_config(node["slurm"]);
    }

    if (node["jobs"] && node["jobs"].IsMap()) {
        for (const auto& job_kv : node["jobs"]) {
            std::string job_name = job_kv.first.as<std::string>();
            JobConfig jc;

            if (job_kv.second.IsMap()) {
                jc.script = job_kv.second["script"].as<std::string>("");
                jc.args = job_kv.second["args"].as<std::string>("");
                jc.time = job_kv.second["time"].as<std::string>("");
                if (job_kv.second["slurm"] && job_kv.second["slurm"].IsMap()) {
                    jc.slurm = parse_slurm_config(job_kv.second["slurm"]);
                }
            }
            project.jobs[job_name] = jc;
        }
    }

    if (node["rodata"]) {
        if (node["rodata"].IsSequence()) {
            project.rodata = node["rodata"].as<std::vector<std::string>>(std::vector<std::string>());
        } else if (node["rodata"].IsScalar()) {
            project.rodata.push_back(node["rodata"].as<std::string>());
        }
    }

    if (node["env_file"]) {
        project.env_file = node["env_file"].as<std::string>("");
    }

    if (node["output"]) {
        project.output = node["output"].as<std::string>("");
    }

    if (node["cache"]) {
        project.cache = node["cache"].as<std::string>("");
    }

    return project;
}

Result<Config> Config::load_global() {
    if (!global_config_exists()) {
        return Result<Config>::Err("Global config not found at " + get_global_config_path().string());
    }

    try {
        YAML::Node root = YAML::LoadFile(get_global_config_path().string());

        Config config;

        // Parse DTN config (fall back to "proxy" key for compat)
        if (root["dtn"]) {
            config.dtn_ = parse_dtn_config(root["dtn"]);
        } else if (root["proxy"]) {
            config.dtn_ = parse_dtn_config(root["proxy"]);
        } else {
            config.dtn_ = parse_dtn_config(YAML::Node());
        }

        // Parse Login config (fall back to "cluster" key for compat)
        if (root["login"]) {
            config.login_ = parse_login_config(root["login"]);
        } else if (root["cluster"]) {
            config.login_ = parse_login_config(root["cluster"]);
        } else {
            config.login_ = parse_login_config(YAML::Node());
        }

        config.slurm_ = parse_slurm_config(root["slurm"] ? root["slurm"] : YAML::Node());
        config.modules_ = root["modules"].as<std::vector<std::string>>(std::vector<std::string>());

        if (root["duo_auto"]) {
            config.duo_auto_ = root["duo_auto"].as<std::string>();
        }

        config.project_dir_ = fs::current_path();

        return Result<Config>::Ok(config);
    } catch (const std::exception& e) {
        return Result<Config>::Err(std::string("Failed to parse global config: ") + e.what());
    }
}

Result<Config> Config::load_project(const fs::path& dir) {
    if (!project_config_exists(dir)) {
        return Result<Config>::Err("Project config not found at " + get_project_config_path(dir).string());
    }

    try {
        YAML::Node root = YAML::LoadFile(get_project_config_path(dir).string());

        Config config;
        config.project_ = parse_project_config(root);
        config.project_dir_ = dir;

        return Result<Config>::Ok(config);
    } catch (const std::exception& e) {
        return Result<Config>::Err(std::string("Failed to parse project config: ") + e.what());
    }
}

Result<Config> Config::load(const fs::path& project_dir) {
    // Load global first
    auto global_result = load_global();
    if (!global_result.is_ok()) {
        return global_result;
    }

    Config config = global_result.value;
    config.project_dir_ = project_dir;

    // Load project if it exists
    if (project_config_exists(project_dir)) {
        auto project_result = load_project(project_dir);
        if (project_result.is_ok()) {
            config.project_ = project_result.value.project_;
        }
    }

    return Result<Config>::Ok(config);
}
