#include "../credentials.hpp"
#include <platform/platform.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

// Credentials stored as simple key=value lines in ~/.tccp/credentials
// File is chmod 600.

static fs::path creds_path() {
    return platform::home_dir() / ".tccp" / "credentials";
}

// Read all key=value pairs from the credentials file
static std::map<std::string, std::string> read_all() {
    std::map<std::string, std::string> m;
    std::ifstream f(creds_path());
    if (!f) return m;

    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            m[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return m;
}

// Write all key=value pairs to the credentials file (chmod 600)
static bool write_all(const std::map<std::string, std::string>& m) {
    auto path = creds_path();
    fs::create_directories(path.parent_path());

    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;

    for (const auto& [k, v] : m) {
        f << k << "=" << v << "\n";
    }
    f.close();

    chmod(path.c_str(), 0600);
    return true;
}

Result<std::string> CredentialManager::get_impl(const std::string& key) {
    auto m = read_all();
    auto it = m.find(key);
    if (it == m.end()) {
        return Result<std::string>::Err("Credential not found");
    }
    return Result<std::string>::Ok(it->second);
}

Result<void> CredentialManager::set_impl(const std::string& key, const std::string& value) {
    auto m = read_all();
    m[key] = value;
    if (!write_all(m)) {
        return Result<void>::Err("Failed to write credentials file");
    }
    return Result<void>::Ok();
}

Result<void> CredentialManager::remove_impl(const std::string& key) {
    auto m = read_all();
    if (m.erase(key) == 0) {
        return Result<void>::Err("Credential not found");
    }
    if (!write_all(m)) {
        return Result<void>::Err("Failed to write credentials file");
    }
    return Result<void>::Ok();
}
