#include "state_store.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <cstdlib>

StateStore::StateStore(const std::string& project_name) {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    state_path_ = fs::path(home) / ".tccp" / "state" / (project_name + ".yaml");
}

ProjectState StateStore::load() {
    ProjectState state;

    if (!fs::exists(state_path_)) {
        return state;
    }

    try {
        YAML::Node root = YAML::LoadFile(state_path_.string());

        if (root["allocations"] && root["allocations"].IsSequence()) {
            for (const auto& n : root["allocations"]) {
                AllocationState a;
                a.slurm_id = n["slurm_id"].as<std::string>("");
                a.node = n["node"].as<std::string>("");
                a.start_time = n["start_time"].as<std::string>("");
                a.duration_minutes = n["duration_minutes"].as<int>(240);
                a.active_job_id = n["active_job_id"].as<std::string>("");
                a.partition = n["partition"].as<std::string>("");
                a.nodes = n["nodes"].as<int>(1);
                a.cpus = n["cpus"].as<int>(1);
                a.memory = n["memory"].as<std::string>("4G");
                a.gpu_type = n["gpu_type"].as<std::string>("");
                a.gpu_count = n["gpu_count"].as<int>(0);
                state.allocations.push_back(a);
            }
        }

        if (root["jobs"] && root["jobs"].IsSequence()) {
            for (const auto& n : root["jobs"]) {
                JobState j;
                j.job_id = n["job_id"].as<std::string>("");
                j.job_name = n["job_name"].as<std::string>("");
                j.alloc_slurm_id = n["alloc_slurm_id"].as<std::string>("");
                j.compute_node = n["compute_node"].as<std::string>("");
                j.completed = n["completed"].as<bool>(false);
                j.canceled = n["canceled"].as<bool>(false);
                j.exit_code = n["exit_code"].as<int>(-1);
                j.output_file = n["output_file"].as<std::string>("");
                j.scratch_path = n["scratch_path"].as<std::string>("");
                j.init_complete = n["init_complete"].as<bool>(false);
                j.init_error = n["init_error"].as<std::string>("");
                j.output_returned = n["output_returned"].as<bool>(false);
                j.submit_time = n["submit_time"].as<std::string>("");
                j.start_time = n["start_time"].as<std::string>("");
                j.end_time = n["end_time"].as<std::string>("");
                state.jobs.push_back(j);
            }
        }

        if (root["last_sync_manifest"] && root["last_sync_manifest"].IsSequence()) {
            for (const auto& n : root["last_sync_manifest"]) {
                SyncManifestEntry e;
                e.path = n["path"].as<std::string>("");
                e.mtime = n["mtime"].as<int64_t>(0);
                e.size = n["size"].as<int64_t>(0);
                state.last_sync_manifest.push_back(e);
            }
        }

        state.last_sync_node = root["last_sync_node"].as<std::string>("");
        state.last_sync_scratch = root["last_sync_scratch"].as<std::string>("");

    } catch (const std::exception&) {
        // Corrupted state file — start fresh
        return ProjectState{};
    }

    return state;
}

void StateStore::save(const ProjectState& state) {
    // Ensure directory exists
    fs::create_directories(state_path_.parent_path());

    YAML::Emitter out;
    out << YAML::BeginMap;

    // Allocations
    out << YAML::Key << "allocations" << YAML::Value << YAML::BeginSeq;
    for (const auto& a : state.allocations) {
        out << YAML::BeginMap;
        out << YAML::Key << "slurm_id" << YAML::Value << a.slurm_id;
        out << YAML::Key << "node" << YAML::Value << a.node;
        out << YAML::Key << "start_time" << YAML::Value << a.start_time;
        out << YAML::Key << "duration_minutes" << YAML::Value << a.duration_minutes;
        out << YAML::Key << "active_job_id" << YAML::Value << a.active_job_id;
        out << YAML::Key << "partition" << YAML::Value << a.partition;
        out << YAML::Key << "nodes" << YAML::Value << a.nodes;
        out << YAML::Key << "cpus" << YAML::Value << a.cpus;
        out << YAML::Key << "memory" << YAML::Value << a.memory;
        out << YAML::Key << "gpu_type" << YAML::Value << a.gpu_type;
        out << YAML::Key << "gpu_count" << YAML::Value << a.gpu_count;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    // Jobs
    out << YAML::Key << "jobs" << YAML::Value << YAML::BeginSeq;
    for (const auto& j : state.jobs) {
        out << YAML::BeginMap;
        out << YAML::Key << "job_id" << YAML::Value << j.job_id;
        out << YAML::Key << "job_name" << YAML::Value << j.job_name;
        out << YAML::Key << "alloc_slurm_id" << YAML::Value << j.alloc_slurm_id;
        out << YAML::Key << "compute_node" << YAML::Value << j.compute_node;
        out << YAML::Key << "completed" << YAML::Value << j.completed;
        out << YAML::Key << "canceled" << YAML::Value << j.canceled;
        out << YAML::Key << "exit_code" << YAML::Value << j.exit_code;
        out << YAML::Key << "output_file" << YAML::Value << j.output_file;
        out << YAML::Key << "scratch_path" << YAML::Value << j.scratch_path;
        out << YAML::Key << "init_complete" << YAML::Value << j.init_complete;
        out << YAML::Key << "init_error" << YAML::Value << j.init_error;
        out << YAML::Key << "output_returned" << YAML::Value << j.output_returned;
        out << YAML::Key << "submit_time" << YAML::Value << j.submit_time;
        out << YAML::Key << "start_time" << YAML::Value << j.start_time;
        out << YAML::Key << "end_time" << YAML::Value << j.end_time;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    // Sync manifest
    out << YAML::Key << "last_sync_manifest" << YAML::Value << YAML::BeginSeq;
    for (const auto& e : state.last_sync_manifest) {
        out << YAML::BeginMap;
        out << YAML::Key << "path" << YAML::Value << e.path;
        out << YAML::Key << "mtime" << YAML::Value << e.mtime;
        out << YAML::Key << "size" << YAML::Value << e.size;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    out << YAML::Key << "last_sync_node" << YAML::Value << state.last_sync_node;
    out << YAML::Key << "last_sync_scratch" << YAML::Value << state.last_sync_scratch;

    out << YAML::EndMap;

    std::ofstream fout(state_path_.string());
    fout << out.c_str();
}
