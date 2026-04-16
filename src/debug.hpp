#pragma once

#include "types.hpp"
#include <fmt/format.h>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

inline bool debug_enabled() {
    static bool enabled = [] {
        const char* v = std::getenv("TCCP_DEBUG");
        return v && *v && std::string(v) != "0";
    }();
    return enabled;
}

inline fs::path debug_log_path() {
    return home_dir() / ".tccp" / "debug.log";
}

inline void debug_log(const std::string& tag, const std::string& msg) {
    if (!debug_enabled()) return;
    static std::mutex m;
    static std::ofstream log;
    static bool opened = false;
    std::lock_guard<std::mutex> lock(m);
    if (!opened) {
        fs::create_directories(debug_log_path().parent_path());
        log.open(debug_log_path(), std::ios::app);
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char ds[64];
        std::strftime(ds, sizeof(ds), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        log << "\n====== tccp session " << ds << " (pid " << getpid() << ") ======\n";
        opened = true;
    }
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    log << ts << "." << fmt::format("{:03}", ms) << " [" << tag << "] " << msg << "\n";
    log.flush();
}

inline std::string debug_truncate(const std::string& s, size_t n = 4000) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + fmt::format("...<+{}B>", s.size() - n);
}
