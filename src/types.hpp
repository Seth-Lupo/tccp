#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ── Result<T> ─────────────────────────────────────────────

template <typename T>
struct Result {
    bool success;
    T value;
    std::string error;

    static Result<T> Ok(T val) { return {true, std::move(val), ""}; }
    static Result<T> Err(const std::string& err) { return {false, T{}, err}; }
    bool is_ok() const { return success; }
    bool is_err() const { return !success; }
};

template <>
struct Result<void> {
    bool success;
    std::string error;

    static Result<void> Ok() { return {true, ""}; }
    static Result<void> Err(const std::string& err) { return {false, err}; }
    bool is_ok() const { return success; }
    bool is_err() const { return !success; }
};

// ── SSH result ────────────────────────────────────────────

struct SSHResult {
    int exit_code;
    std::string out, err;
    bool ok() const { return exit_code == 0; }
};

// ── Manifest ──────────────────────────────────────────────

struct ManifestEntry {
    std::string path;
    int64_t mtime;
    int64_t size;
};

// ── Config structs ────────────────────────────────────────

struct ProjectConfig {
    std::string host;
    std::string login;
    std::string partition;
    std::string container;
    std::string init;
    std::string gpu;
    int gpu_count = 1;
    int cpus = 4;
    std::string memory = "32G";
    std::string time = "4h";
    std::string output = "output/";
    std::vector<int> ports;
    std::vector<std::string> rodata;
};

struct GlobalConfig {
    std::string host = "xfer.cluster.tufts.edu";
    std::string login = "login.pax.tufts.edu";
    std::string user;
    std::string password;
    std::string partition = "gpu";
    std::string gpu;
    int gpu_count = 1;
    int cpus = 4;
    std::string memory = "32G";
    std::string time = "4h";
    bool cache_containers = false;
};

struct Config {
    GlobalConfig global;
    ProjectConfig project;
    std::string project_name;
    fs::path project_dir;
};

// ── Session state ─────────────────────────────────────────

struct SessionState {
    std::string slurm_id;
    std::string compute_node;
    std::string partition;
    std::string scratch;
    std::string container_uri;
    std::string container_sif;
    std::string started_at;
    std::vector<ManifestEntry> manifest;
};

// ── Utilities ─────────────────────────────────────────────

inline fs::path home_dir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) return fs::temp_directory_path();
    return fs::path(home);
}

inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

using StatusCallback = std::function<void(const std::string&)>;
