#include "platform.hpp"
#include <cstdlib>
#include <ctime>
#include <random>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace platform {

fs::path home_dir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) return temp_dir();
    return fs::path(home);
}

fs::path temp_dir() {
    return fs::temp_directory_path();
}

fs::path temp_file(const std::string& prefix) {
#ifdef _WIN32
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t fname[MAX_PATH];
    GetTempFileNameW(tmp, L"tccp", 0, fname);
    fs::path result(fname);
    // Rename with prefix for clarity
    fs::path renamed = result.parent_path() / (prefix + "_" + result.filename().string());
    fs::rename(result, renamed);
    return renamed;
#else
    // Use pid + random for uniqueness
    static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
    std::uniform_int_distribution<int> dist(10000, 99999);
    fs::path p = temp_dir() / (prefix + "_" + std::to_string(dist(rng)));
    return p;
#endif
}

void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

} // namespace platform
