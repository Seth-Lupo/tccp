#include "config.hpp"
#include <managers/gpu_discovery.hpp>
#include <platform/platform.hpp>
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

// ── GPU shorthand parsing ─────────────────────────────────────
// Accepts: "a100", "a100:2", "a100-80gb", "2" (count only)
// Bare base types like "a100" are resolved to the cheapest variant (lowest tier).
static void parse_gpu_shorthand(const std::string& value,
                                std::string& out_type, int& out_count) {
    // Explicit opt-out
    if (value == "none" || value == "false") {
        out_type = "";
        out_count = 0;
        return;
    }

    auto colon = value.find(':');
    if (colon != std::string::npos) {
        out_type = value.substr(0, colon);
        try { out_count = std::stoi(value.substr(colon + 1)); } catch (...) { out_count = 1; }
    } else {
        // Pure number → count only; otherwise it's a type name
        try {
            out_count = std::stoi(value);
        } catch (...) {
            out_type = value;
            if (out_count <= 0) out_count = 1;
        }
    }

    // If the type is a bare base (e.g. "a100") with known variants, resolve to
    // the cheapest one (lowest tier) so downstream defaults and matching work.
    if (!out_type.empty() && !find_variant_by_id(out_type)) {
        auto variants = find_variants_by_base(out_type);
        if (!variants.empty()) {
            const GpuVariant* cheapest = variants[0];
            for (const auto* v : variants) {
                if (v->tier < cheapest->tier) cheapest = v;
            }
            out_type = cheapest->id;
        }
    }
}

// Apply intelligent defaults when a GPU is requested but memory/cpus weren't
// explicitly set.  This prevents the common mistake of requesting an A100 with 4G RAM.
static void apply_gpu_smart_defaults(SlurmDefaults& s, bool memory_set, bool cpus_set) {
    if (s.gpu_type.empty() && s.gpu_count <= 0) return;

    if (!cpus_set || s.cpus_per_task <= 1) {
        s.cpus_per_task = 4;
    }

    if (!memory_set || s.memory.empty() || s.memory == "4G") {
        // Scale memory based on GPU variant
        const auto* variant = find_variant_by_id(s.gpu_type);
        if (variant && variant->mem_gb >= 80) {
            s.memory = "64G";   // A100-80GB class
        } else {
            s.memory = "32G";   // A100-40GB, V100, etc.
        }
    }
}

// Read inline resource keys (gpu, memory, cpus, partition) from a YAML node
// into a SlurmDefaults, tracking which fields were explicitly set.
// Check if a YAML node has any inline resource shorthand keys.
static bool has_inline_resources(const YAML::Node& node) {
    return (node["gpu"] && node["gpu"].IsScalar()) ||
           (node["gpu_count"] && node["gpu_count"].IsScalar()) ||
           (node["memory"] && node["memory"].IsScalar()) ||
           (node["cpus"] && node["cpus"].IsScalar()) ||
           (node["cpu_count"] && node["cpu_count"].IsScalar()) ||
           (node["time"] && node["time"].IsScalar()) ||
           (node["partition"] && node["partition"].IsScalar());
}

static void overlay_inline_resources(const YAML::Node& node, SlurmDefaults& s,
                                     bool& memory_set, bool& cpus_set) {
    if (node["gpu"] && node["gpu"].IsScalar()) {
        parse_gpu_shorthand(node["gpu"].as<std::string>(), s.gpu_type, s.gpu_count);
    }
    if (node["gpu_count"] && node["gpu_count"].IsScalar()) {
        s.gpu_count = node["gpu_count"].as<int>(1);
    }
    if (node["memory"] && node["memory"].IsScalar()) {
        s.memory = node["memory"].as<std::string>();
        memory_set = true;
    }
    if (node["cpus"] && node["cpus"].IsScalar()) {
        s.cpus_per_task = node["cpus"].as<int>(1);
        cpus_set = true;
    }
    if (node["cpu_count"] && node["cpu_count"].IsScalar()) {
        s.cpus_per_task = node["cpu_count"].as<int>(1);
        cpus_set = true;
    }
    if (node["time"] && node["time"].IsScalar()) {
        s.time = node["time"].as<std::string>();
    }
    if (node["partition"] && node["partition"].IsScalar()) {
        s.partition = node["partition"].as<std::string>();
    }
    if (node["exclude_nodes"] && node["exclude_nodes"].IsScalar()) {
        s.exclude_nodes = node["exclude_nodes"].as<std::string>();
    }
}

bool global_config_exists() {
    return fs::exists(get_global_config_path());
}

bool project_config_exists(const fs::path& dir) {
    return fs::exists(get_project_config_path(dir));
}

fs::path get_global_config_dir() {
    return platform::home_dir() / ".tccp";
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
  memory: "4G"
  cpus_per_task: 1

# Optional: Environment modules to load
modules: []

# Optional: Text editor for viewing output (default: vim)
# editor: "vim"

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
    slurm.exclude_nodes = node["exclude_nodes"].as<std::string>("");

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

    // Overlay top-level resource keys on top of any existing slurm block.
    // Creates one if needed.
    if (has_inline_resources(node)) {
        if (!project.slurm.has_value()) {
            SlurmDefaults slurm;
            slurm.partition = "batch";
            slurm.time = "00:30:00";
            slurm.nodes = 1;
            slurm.cpus_per_task = 1;
            slurm.memory = "4G";
            slurm.gpu_count = 0;
            slurm.mail_type = "NONE";
            project.slurm = slurm;
        }
        bool mem_set = false, cpus_set = false;
        // If slurm: block already set memory/cpus, mark as explicit
        if (node["slurm"] && node["slurm"].IsMap()) {
            if (node["slurm"]["memory"]) mem_set = true;
            if (node["slurm"]["cpus_per_task"]) cpus_set = true;
        }
        overlay_inline_resources(node, project.slurm.value(), mem_set, cpus_set);
        apply_gpu_smart_defaults(project.slurm.value(), mem_set, cpus_set);
    } else if (project.slurm.has_value()) {
        // Even with just a slurm: block, apply smart GPU defaults
        bool mem_set = node["slurm"] && node["slurm"]["memory"];
        bool cpus_set = node["slurm"] && node["slurm"]["cpus_per_task"];
        apply_gpu_smart_defaults(project.slurm.value(), mem_set, cpus_set);
    }

    if (node["jobs"] && node["jobs"].IsMap()) {
        for (const auto& job_kv : node["jobs"]) {
            std::string job_name = job_kv.first.as<std::string>();
            JobConfig jc;

            if (job_kv.second.IsScalar()) {
                // Bare string shorthand: `train: train.py`
                jc.script = job_kv.second.as<std::string>("");
            } else if (job_kv.second.IsMap()) {
                const auto& jnode = job_kv.second;
                jc.script = jnode["script"].as<std::string>("");
                jc.package = jnode["package"].as<std::string>("");
                jc.args = jnode["args"].as<std::string>("");
                jc.time = jnode["time"].as<std::string>("");

                auto ports_node = jnode["ports"] ? jnode["ports"] : jnode["port"];
                if (ports_node) {
                    if (ports_node.IsScalar()) {
                        jc.ports.push_back(ports_node.as<int>());
                    } else if (ports_node.IsSequence()) {
                        for (const auto& p : ports_node)
                            jc.ports.push_back(p.as<int>());
                    }
                }

                // Full slurm: block (backwards compat)
                if (jnode["slurm"] && jnode["slurm"].IsMap()) {
                    jc.slurm = parse_slurm_config(jnode["slurm"]);
                }

                // Inline resource keys
                if (has_inline_resources(jnode)) {
                    if (!jc.slurm.has_value()) {
                        jc.slurm = SlurmDefaults{};
                    }
                    bool mem_set = false, cpus_set = false;
                    if (jnode["slurm"] && jnode["slurm"].IsMap()) {
                        if (jnode["slurm"]["memory"]) mem_set = true;
                        if (jnode["slurm"]["cpus_per_task"]) cpus_set = true;
                    }
                    overlay_inline_resources(jnode, jc.slurm.value(), mem_set, cpus_set);
                    apply_gpu_smart_defaults(jc.slurm.value(), mem_set, cpus_set);
                } else if (jc.slurm.has_value()) {
                    bool mem_set = jnode["slurm"] && jnode["slurm"]["memory"];
                    bool cpus_set = jnode["slurm"] && jnode["slurm"]["cpus_per_task"];
                    apply_gpu_smart_defaults(jc.slurm.value(), mem_set, cpus_set);
                }
            }
            project.jobs[job_name] = jc;
        }
    }

    // Top-level `script` shorthand: if no jobs defined, create a "main" job
    if (project.jobs.empty()) {
        JobConfig jc;
        jc.script = node["script"].as<std::string>("main.py");
        jc.args = node["args"].as<std::string>("");
        project.jobs["main"] = jc;
    }

    // Top-level `port`/`ports` shorthand: apply to any job without its own ports
    auto top_ports_node = node["ports"] ? node["ports"] : node["port"];
    if (top_ports_node) {
        std::vector<int> default_ports;
        if (top_ports_node.IsScalar()) {
            default_ports.push_back(top_ports_node.as<int>());
        } else if (top_ports_node.IsSequence()) {
            for (const auto& p : top_ports_node)
                default_ports.push_back(p.as<int>());
        }
        if (!default_ports.empty()) {
            for (auto& [name, jc] : project.jobs) {
                if (jc.ports.empty()) jc.ports = default_ports;
            }
        }
    }

    if (node["rodata"]) {
        if (node["rodata"].IsSequence()) {
            project.rodata = node["rodata"].as<std::vector<std::string>>(std::vector<std::string>());
        } else if (node["rodata"].IsScalar()) {
            project.rodata.push_back(node["rodata"].as<std::string>());
        }
    }

    // env: accept "env" or legacy "env_file"
    if (node["env"]) {
        project.env = node["env"].as<std::string>(".env");
    } else if (node["env_file"]) {
        project.env = node["env_file"].as<std::string>(".env");
    } else {
        project.env = ".env";
    }

    project.output = node["output"].as<std::string>("output");
    project.cache = node["cache"].as<std::string>("cache");

    // Environment-type defaults: python-pytorch gets a GPU if none was specified.
    // An explicit `gpu: none` counts as "specified" and opts out.
    if (project.type == "python-pytorch") {
        bool gpu_explicitly_set = (node["gpu"] && node["gpu"].IsScalar());
        bool slurm_has_gpu = project.slurm.has_value() &&
            (!project.slurm->gpu_type.empty() || project.slurm->gpu_count > 0);
        if (!gpu_explicitly_set && !slurm_has_gpu) {
            if (!project.slurm.has_value()) {
                project.slurm = SlurmDefaults{};
                project.slurm->partition = "batch";
                project.slurm->time = "00:30:00";
                project.slurm->nodes = 1;
                project.slurm->cpus_per_task = 1;
                project.slurm->memory = "4G";
                project.slurm->gpu_count = 0;
                project.slurm->mail_type = "NONE";
            }
            project.slurm->gpu_type = "t4";
            project.slurm->gpu_count = 1;
            apply_gpu_smart_defaults(project.slurm.value(), false, false);
        }
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

#ifdef _WIN32
        const char* default_editor = "notepad";
#else
        const char* default_editor = "vim";
#endif
        config.editor_ = root["editor"].as<std::string>(default_editor);

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

        // Auto-infer project name from directory name if not set
        if (config.project_.name.empty()) {
            config.project_.name = dir.filename().string();
        }

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

    // Auto-infer project name from directory name if not set
    if (config.project_.name.empty()) {
        config.project_.name = project_dir.filename().string();
    }

    return Result<Config>::Ok(config);
}
