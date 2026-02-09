#include "../credentials.hpp"
#include <wincred.h>
#include <windows.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

Result<std::string> CredentialManager::get_impl(const std::string& key) {
    PCREDENTIAL cred = nullptr;
    std::wstring wkey(key.begin(), key.end());

    BOOL success = CredRead(wkey.c_str(), CRED_TYPE_GENERIC, 0, &cred);

    if (!success) {
        return Result<std::string>::Err("Credential not found in Windows Credential Manager");
    }

    std::string result;
    if (cred->CredentialBlob) {
        result = std::string(
            reinterpret_cast<char*>(cred->CredentialBlob),
            cred->CredentialBlobSize
        );
    }

    CredFree(cred);
    return Result<std::string>::Ok(result);
}

Result<void> CredentialManager::set_impl(const std::string& key, const std::string& value) {
    std::wstring wkey(key.begin(), key.end());

    CREDENTIAL cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(wkey.c_str());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.c_str()));
    cred.CredentialBlobSize = static_cast<DWORD>(value.length());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = L"tccp";

    BOOL success = CredWrite(&cred, 0);

    if (!success) {
        return Result<void>::Err("Failed to store credential in Windows Credential Manager");
    }

    return Result<void>::Ok();
}

Result<void> CredentialManager::remove_impl(const std::string& key) {
    std::wstring wkey(key.begin(), key.end());

    BOOL success = CredDelete(wkey.c_str(), CRED_TYPE_GENERIC, 0);

    if (!success) {
        return Result<void>::Err("Failed to delete credential");
    }

    return Result<void>::Ok();
}
