#pragma once

#include <string>
#include <optional>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>

// Result type for operations that can fail
template <typename T>
struct Result {
    bool success;
    T value;
    std::string error;

    static Result<T> Ok(T val) {
        return {true, std::move(val), ""};
    }

    static Result<T> Err(const std::string& err) {
        return {false, T{}, err};
    }

    bool is_ok() const { return success; }
    bool is_err() const { return !success; }
};

// Specialization for void
template <>
struct Result<void> {
    bool success;
    std::string error;

    static Result<void> Ok() {
        return {true, ""};
    }

    static Result<void> Err(const std::string& err) {
        return {false, err};
    }

    bool is_ok() const { return success; }
    bool is_err() const { return !success; }
};

// SSH command execution result
struct SSHResult {
    int exit_code;
    std::string stdout_data;
    std::string stderr_data;

    bool success() const { return exit_code == 0; }
    bool failed() const { return exit_code != 0; }

    std::string get_output() const {
        return stdout_data.empty() ? stderr_data : stdout_data;
    }
};

// Configuration structures
struct DTNConfig {
    std::string host;
    std::string user;
    std::optional<std::string> password;
    int timeout = 30;
    std::optional<std::string> ssh_key_path;
};

struct LoginConfig {
    std::string host;
    std::string user;
    std::string password;
    std::string email;
    int timeout = 30;
    std::string remote_dir;
    std::optional<std::string> ssh_key_path;
};

struct SlurmDefaults {
    std::string partition;
    std::string time;
    int nodes;
    int cpus_per_task;
    std::string memory;
    std::string gpu_type;
    int gpu_count;
    std::string mail_type;
    std::string node_constraint;  // node prefix for --nodelist (e.g. "cc1gpu")
    std::string exclude_nodes;    // nodes to exclude (e.g. "s1cmp003,s1cmp004")
};

struct JobConfig {
    std::string script;                          // python script to run (e.g. "train.py")
    std::string package;                         // python package to run with -m (e.g. "myapp")
    std::string args;
    std::string time;                            // job time limit, default "1:00:00"
    std::optional<SlurmDefaults> slurm;          // per-job SLURM overrides
    std::vector<int> ports;                      // localhost ports to forward to compute node
};

struct ProjectConfig {
    std::string name;
    std::string type = "python";
    std::string description;
    std::string remote_dir;
    std::map<std::string, std::string> environment;
    std::map<std::string, JobConfig> jobs;
    std::vector<std::string> rodata;             // read-only data directories (string or list in YAML)
    std::string env;                              // env file to sync (bypasses gitignore)
    std::string output;                          // output directory
    std::string cache;                           // cache directory (best-effort, shared across jobs on same allocation)
    std::optional<SlurmDefaults> slurm;          // project-level SLURM defaults
};

// Status callback for operations
using StatusCallback = std::function<void(const std::string&)>;
