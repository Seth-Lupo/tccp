#include "allocation_store.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <cstdlib>

AllocationStore::AllocationStore(const std::string& cluster_name)
    : cluster_name_(cluster_name) {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    store_path_ = fs::path(home) / ".tccp" / "allocations" / (cluster_name + ".yaml");
}

ClusterAllocations AllocationStore::load() {
    ClusterAllocations allocs;

    if (!fs::exists(store_path_)) {
        return allocs;
    }

    try {
        YAML::Node root = YAML::LoadFile(store_path_.string());

        if (root["allocations"] && root["allocations"].IsSequence()) {
            for (const auto& n : root["allocations"]) {
                AllocationState a;
                a.slurm_id = n["slurm_id"].as<std::string>("");
                a.node = n["node"].as<std::string>("");
                a.start_time = n["start_time"].as<std::string>("");
                a.duration_minutes = n["duration_minutes"].as<int>(240);
                a.project_name = n["project_name"].as<std::string>("");
                a.active_job_id = n["active_job_id"].as<std::string>("");
                allocs.allocations.push_back(a);
            }
        }
    } catch (const std::exception&) {
        // Corrupted state file — start fresh
        return ClusterAllocations{};
    }

    return allocs;
}

void AllocationStore::save(const ClusterAllocations& allocs) {
    // Ensure directory exists
    fs::create_directories(store_path_.parent_path());

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "allocations" << YAML::Value << YAML::BeginSeq;

    for (const auto& a : allocs.allocations) {
        out << YAML::BeginMap;
        out << YAML::Key << "slurm_id" << YAML::Value << a.slurm_id;
        out << YAML::Key << "node" << YAML::Value << a.node;
        out << YAML::Key << "start_time" << YAML::Value << a.start_time;
        out << YAML::Key << "duration_minutes" << YAML::Value << a.duration_minutes;
        out << YAML::Key << "project_name" << YAML::Value << a.project_name;
        out << YAML::Key << "active_job_id" << YAML::Value << a.active_job_id;
        out << YAML::EndMap;
    }

    out << YAML::EndSeq;
    out << YAML::EndMap;

    std::ofstream fout(store_path_.string());
    fout << out.c_str();
}
