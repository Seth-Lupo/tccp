#include "utils.hpp"
#include "credentials.hpp"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <stdexcept>

std::string get_cluster_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "unknown";
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(buf);
}

std::time_t parse_iso_time(const std::string& iso) {
    struct tm tm_buf = {};
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
               &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec) == 6) {
        tm_buf.tm_year -= 1900;
        tm_buf.tm_mon -= 1;
        return mktime(&tm_buf);
    }
    return 0;
}

int safe_stoi(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}
