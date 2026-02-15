#pragma once

#include <string>
#include <fstream>
#include <chrono>
#include <ctime>
#include <ssh/connection.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>

static const char* TCCP_LOG_PATH = "/tmp/tccp_debug.log";

// now_iso() is provided by core/utils.hpp — no duplicate here.

inline void tccp_log(const std::string& msg) {
    std::ofstream out(TCCP_LOG_PATH, std::ios::app);
    if (!out) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

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
