#include "job_metadata.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <cstdlib>
#include <sstream>

// ── Path helpers ────────────────────────────────────────────

fs::path JobMetadata::local_jobs_dir(const std::string& project_name) {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path(home) / ".tccp" / "projects" / project_name / "jobs";
}

fs::path JobMetadata::local_metadata_path(const std::string& project_name, const std::string& job_id) {
    return local_jobs_dir(project_name) / (job_id + ".yaml");
}

std::string JobMetadata::remote_metadata_path(const std::string& project_name,
                                               const std::string& username,
                                               const std::string& job_id) {
    return "/cluster/home/" + username + "/tccp/projects/" + project_name + "/jobs/" + job_id + "/metadata.yaml";
}

// ── Load local ──────────────────────────────────────────────

Result<JobMetadata> JobMetadata::load_local(const std::string& project_name, const std::string& job_id) {
    auto path = local_metadata_path(project_name, job_id);

    if (!fs::exists(path)) {
        return Result<JobMetadata>::Err("Local metadata not found: " + path.string());
    }

    try {
        YAML::Node root = YAML::LoadFile(path.string());

        JobMetadata meta;
        meta.job_id = root["job_id"].as<std::string>("");
        meta.job_name = root["job_name"].as<std::string>("");
        meta.submit_time = root["submit_time"].as<std::string>("");
        meta.start_time = root["start_time"].as<std::string>("");
        meta.end_time = root["end_time"].as<std::string>("");
        meta.slurm_id = root["slurm_id"].as<std::string>("");
        meta.compute_node = root["compute_node"].as<std::string>("");
        meta.scratch_path = root["scratch_path"].as<std::string>("");
        meta.exit_code = root["exit_code"].as<int>(-1);
        meta.completed = root["completed"].as<bool>(false);
        meta.canceled = root["canceled"].as<bool>(false);
        meta.init_complete = root["init_complete"].as<bool>(false);
        meta.init_error = root["init_error"].as<std::string>("");

        return Result<JobMetadata>::Ok(meta);
    } catch (const std::exception& e) {
        return Result<JobMetadata>::Err("Failed to load local metadata: " + std::string(e.what()));
    }
}

// ── Load remote ─────────────────────────────────────────────

Result<JobMetadata> JobMetadata::load_remote(SSHConnection& dtn, const std::string& project_name,
                                              const std::string& username, const std::string& job_id) {
    std::string remote_path = remote_metadata_path(project_name, username, job_id);

    // Check if file exists
    auto check = dtn.run("test -f " + remote_path + " && echo EXISTS || echo MISSING");
    if (check.stdout_data.find("MISSING") != std::string::npos) {
        return Result<JobMetadata>::Err("Remote metadata not found");
    }

    // Read file content
    auto result = dtn.run("cat " + remote_path);
    if (result.exit_code != 0) {
        return Result<JobMetadata>::Err("Failed to read remote metadata");
    }

    try {
        YAML::Node root = YAML::Load(result.stdout_data);

        JobMetadata meta;
        meta.job_id = root["job_id"].as<std::string>("");
        meta.job_name = root["job_name"].as<std::string>("");
        meta.submit_time = root["submit_time"].as<std::string>("");
        meta.start_time = root["start_time"].as<std::string>("");
        meta.end_time = root["end_time"].as<std::string>("");
        meta.slurm_id = root["slurm_id"].as<std::string>("");
        meta.compute_node = root["compute_node"].as<std::string>("");
        meta.scratch_path = root["scratch_path"].as<std::string>("");
        meta.exit_code = root["exit_code"].as<int>(-1);
        meta.completed = root["completed"].as<bool>(false);
        meta.canceled = root["canceled"].as<bool>(false);
        meta.init_complete = root["init_complete"].as<bool>(false);
        meta.init_error = root["init_error"].as<std::string>("");

        return Result<JobMetadata>::Ok(meta);
    } catch (const std::exception& e) {
        return Result<JobMetadata>::Err("Failed to parse remote metadata: " + std::string(e.what()));
    }
}

// ── Save local ──────────────────────────────────────────────

void JobMetadata::save_local(const std::string& project_name) const {
    auto path = local_metadata_path(project_name, job_id);

    // Ensure directory exists
    fs::create_directories(path.parent_path());

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "job_id" << YAML::Value << job_id;
    out << YAML::Key << "job_name" << YAML::Value << job_name;
    out << YAML::Key << "submit_time" << YAML::Value << submit_time;
    out << YAML::Key << "start_time" << YAML::Value << start_time;
    out << YAML::Key << "end_time" << YAML::Value << end_time;
    out << YAML::Key << "slurm_id" << YAML::Value << slurm_id;
    out << YAML::Key << "compute_node" << YAML::Value << compute_node;
    out << YAML::Key << "scratch_path" << YAML::Value << scratch_path;
    out << YAML::Key << "exit_code" << YAML::Value << exit_code;
    out << YAML::Key << "completed" << YAML::Value << completed;
    out << YAML::Key << "canceled" << YAML::Value << canceled;
    out << YAML::Key << "init_complete" << YAML::Value << init_complete;
    out << YAML::Key << "init_error" << YAML::Value << init_error;
    out << YAML::EndMap;

    std::ofstream fout(path.string());
    fout << out.c_str();
}

// ── Save remote ─────────────────────────────────────────────

void JobMetadata::save_remote(SSHConnection& dtn, const std::string& project_name, const std::string& username) const {
    std::string remote_path = remote_metadata_path(project_name, username, job_id);
    std::string remote_dir = "/cluster/home/" + username + "/tccp/projects/" + project_name + "/jobs/" + job_id;

    // Ensure directory exists
    dtn.run("mkdir -p " + remote_dir);

    // Generate YAML content
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "job_id" << YAML::Value << job_id;
    out << YAML::Key << "job_name" << YAML::Value << job_name;
    out << YAML::Key << "submit_time" << YAML::Value << submit_time;
    out << YAML::Key << "start_time" << YAML::Value << start_time;
    out << YAML::Key << "end_time" << YAML::Value << end_time;
    out << YAML::Key << "slurm_id" << YAML::Value << slurm_id;
    out << YAML::Key << "compute_node" << YAML::Value << compute_node;
    out << YAML::Key << "scratch_path" << YAML::Value << scratch_path;
    out << YAML::Key << "exit_code" << YAML::Value << exit_code;
    out << YAML::Key << "completed" << YAML::Value << completed;
    out << YAML::Key << "canceled" << YAML::Value << canceled;
    out << YAML::Key << "init_complete" << YAML::Value << init_complete;
    out << YAML::Key << "init_error" << YAML::Value << init_error;
    out << YAML::EndMap;

    std::string yaml_content = out.c_str();

    // Write via heredoc to avoid quoting issues
    std::string cmd = "cat > " + remote_path + " << 'TCCP_META_EOF'\n" + yaml_content + "\nTCCP_META_EOF";
    dtn.run(cmd);
}

// ── Reconcile ───────────────────────────────────────────────

JobMetadata JobMetadata::reconcile(const JobMetadata& local, const Result<JobMetadata>& remote_result) {
    JobMetadata merged = local;  // Start with local

    if (remote_result.is_ok()) {
        const auto& remote = remote_result.value;

        // Remote is truth for running state
        merged.completed = remote.completed;
        merged.exit_code = remote.exit_code;
        merged.end_time = remote.end_time;
        merged.compute_node = remote.compute_node;
        merged.slurm_id = remote.slurm_id;

        // If remote has start_time and local doesn't, use remote's
        if (!remote.start_time.empty() && merged.start_time.empty()) {
            merged.start_time = remote.start_time;
        }
    }

    // Local-only fields always come from local (init state, submit time)
    // These are already in merged since we started with local

    return merged;
}
