#pragma once

#include <string>
#include <vector>
#include <map>
#include "types.hpp"

struct CredentialInfo {
    std::string key;
    bool has_value;
};

class CredentialManager {
public:
    static CredentialManager& instance();

    // Get credential by key
    Result<std::string> get(const std::string& key);

    // Set credential (store securely in platform keyring)
    Result<void> set(const std::string& key, const std::string& value);

    // Remove credential
    Result<void> remove(const std::string& key);

    // List all stored credentials
    std::vector<CredentialInfo> list();

    // Check if using system keyring
    bool uses_keyring() const;

private:
    CredentialManager() = default;

    // Platform-specific implementations
    Result<std::string> get_impl(const std::string& key);
    Result<void> set_impl(const std::string& key, const std::string& value);
    Result<void> remove_impl(const std::string& key);

    bool keyring_available_;
};
