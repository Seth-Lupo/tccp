#include "../credentials.hpp"
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

// Fallback: store in ~/.tccp/.credentials (with warning)
// In production, would use libsecret for full GNOME Keyring integration

static fs::path get_credentials_path() {
    return fs::home_path() / ".tccp" / ".credentials";
}

static fs::path get_credential_file(const std::string& key) {
    fs::path dir = get_credentials_path();
    fs::create_directories(dir);
    return dir / key;
}

Result<std::string> CredentialManager::get_impl(const std::string& key) {
    try {
        fs::path cred_file = get_credential_file(key);

        if (!fs::exists(cred_file)) {
            return Result<std::string>::Err("Credential not found");
        }

        std::ifstream file(cred_file);
        std::string value((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

        return Result<std::string>::Ok(value);
    } catch (const std::exception& e) {
        return Result<std::string>::Err(std::string("Failed to read credential: ") + e.what());
    }
}

Result<void> CredentialManager::set_impl(const std::string& key, const std::string& value) {
    try {
        fs::path cred_file = get_credential_file(key);

        // Create parent directory
        fs::create_directories(cred_file.parent_path());

        // Write with restricted permissions
        std::ofstream file(cred_file);
        file << value;
        file.close();

        // Restrict to owner only (mode 600)
        fs::permissions(cred_file,
                       fs::perms::owner_read | fs::perms::owner_write,
                       fs::perm_options::replace);

        return Result<void>::Ok();
    } catch (const std::exception& e) {
        return Result<void>::Err(std::string("Failed to store credential: ") + e.what());
    }
}

Result<void> CredentialManager::remove_impl(const std::string& key) {
    try {
        fs::path cred_file = get_credential_file(key);

        if (!fs::exists(cred_file)) {
            return Result<void>::Err("Credential not found");
        }

        fs::remove(cred_file);
        return Result<void>::Ok();
    } catch (const std::exception& e) {
        return Result<void>::Err(std::string("Failed to delete credential: ") + e.what());
    }
}
