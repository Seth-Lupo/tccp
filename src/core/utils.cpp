#include "utils.hpp"
#include "credentials.hpp"
#include <platform/platform.hpp>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <array>

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

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; i += 3) {
        unsigned val = data[i] << 16;
        if (i + 1 < len) val |= data[i + 1] << 8;
        if (i + 2 < len) val |= data[i + 2];
        out += B64_CHARS[(val >> 18) & 0x3F];
        out += B64_CHARS[(val >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(val >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[val & 0x3F] : '=';
    }
    return out;
}

std::string compute_file_md5(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return "";
    }

    // Use platform-specific MD5 command
#ifdef __APPLE__
    const char* md5_cmd = "md5 -q";
#else
    const char* md5_cmd = "md5sum";
#endif

    std::string cmd = std::string(md5_cmd) + " \"" + path.string() + "\"";
    std::array<char, 128> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);

    // Extract just the MD5 hash (first 32 hex chars)
    // md5sum format: "hash  filename"
    // md5 -q format: "hash"
    trim(result);
    size_t space_pos = result.find(' ');
    if (space_pos != std::string::npos) {
        result = result.substr(0, space_pos);
    }

    return result;
}
