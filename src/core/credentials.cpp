#include "credentials.hpp"

CredentialManager& CredentialManager::instance() {
    static CredentialManager mgr;
    return mgr;
}

Result<std::string> CredentialManager::get(const std::string& key) {
    return get_impl(key);
}

Result<void> CredentialManager::set(const std::string& key, const std::string& value) {
    return set_impl(key, value);
}

Result<void> CredentialManager::remove(const std::string& key) {
    return remove_impl(key);
}

std::vector<CredentialInfo> CredentialManager::list() {
    std::vector<CredentialInfo> infos;
    // Platform-specific implementation would populate this
    return infos;
}
