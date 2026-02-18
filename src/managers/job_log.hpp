#pragma once

#include <string>
#include <fstream>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <ssh/connection.hpp>
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <fmt/format.h>

inline std::string tccp_log_path() {
    static std::string path = (platform::temp_dir() / "tccp_debug.log").string();
    return path;
}

// now_iso() is provided by core/utils.hpp — no duplicate here.

// Persistent job log path: ~/.tccp/logs/{job_id}.log
inline std::string job_log_path(const std::string& job_id) {
    return (platform::home_dir() / ".tccp" / "logs" / (job_id + ".log")).string();
}

// Append a timestamped line to a job's persistent log file.
inline void append_job_log(const std::string& job_id, const std::string& msg) {
    std::string path = job_log_path(job_id);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::app);
    if (f) {
        f << "[" << now_iso() << "] " << msg << "\n";
    }
}

inline void tccp_log(const std::string& msg) {
    std::ofstream out(tccp_log_path(), std::ios::app);
    if (!out) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char ts[32];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    out << "[" << ts << "] " << msg << "\n";
}

inline void tccp_log_ssh(const std::string& label, const std::string& cmd,
                          const SSHResult& r) {
    tccp_log(fmt::format("{} CMD: {}", label, cmd));
    tccp_log(fmt::format("{} exit={} stdout({})={}", label, r.exit_code,
                         r.stdout_data.size(), r.stdout_data.substr(0, 500)));
    if (!r.stderr_data.empty())
        tccp_log(fmt::format("{} stderr={}", label, r.stderr_data.substr(0, 500)));
}
