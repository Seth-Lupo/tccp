#include "state.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>

StateStore::StateStore(const std::string& project_name) {
    state_path_ = home_dir() / ".tccp" / "projects" / project_name / "session.yaml";
}

bool StateStore::exists() const {
    return fs::exists(state_path_);
}

SessionState StateStore::load() {
    SessionState state;

    try {
        if (!fs::exists(state_path_)) return state;

        YAML::Node root = YAML::LoadFile(state_path_.string());
        state.slurm_id = root["slurm_id"].as<std::string>("");
        state.compute_node = root["compute_node"].as<std::string>("");
        state.partition = root["partition"].as<std::string>("");
        state.scratch = root["scratch"].as<std::string>("");
        state.container_uri = root["container_uri"].as<std::string>("");
        state.container_sif = root["container_sif"].as<std::string>("");
        state.started_at = root["started_at"].as<std::string>("");

        if (root["manifest"] && root["manifest"].IsSequence()) {
            for (const auto& n : root["manifest"]) {
                ManifestEntry e;
                e.path = n["path"].as<std::string>("");
                e.mtime = n["mtime"].as<int64_t>(0);
                e.size = n["size"].as<int64_t>(0);
                state.manifest.push_back(e);
            }
        }
    } catch (...) {
        return SessionState{};
    }

    return state;
}

void StateStore::save(const SessionState& state) {
    fs::create_directories(state_path_.parent_path());

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "slurm_id" << YAML::Value << state.slurm_id;
    out << YAML::Key << "compute_node" << YAML::Value << state.compute_node;
    out << YAML::Key << "partition" << YAML::Value << state.partition;
    out << YAML::Key << "scratch" << YAML::Value << state.scratch;
    out << YAML::Key << "container_uri" << YAML::Value << state.container_uri;
    out << YAML::Key << "container_sif" << YAML::Value << state.container_sif;
    out << YAML::Key << "started_at" << YAML::Value << state.started_at;

    if (!state.manifest.empty()) {
        out << YAML::Key << "manifest" << YAML::Value << YAML::BeginSeq;
        for (const auto& e : state.manifest) {
            out << YAML::BeginMap;
            out << YAML::Key << "path" << YAML::Value << e.path;
            out << YAML::Key << "mtime" << YAML::Value << e.mtime;
            out << YAML::Key << "size" << YAML::Value << e.size;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }

    out << YAML::EndMap;

    std::ofstream fout(state_path_.string());
    fout << out.c_str();
}

void StateStore::clear() {
    std::error_code ec;
    fs::remove(state_path_, ec);
}
