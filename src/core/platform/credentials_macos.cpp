#include "../credentials.hpp"
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

static CFStringRef cf_str(const std::string& s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

static std::string cf_data_to_string(CFDataRef data) {
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                       CFDataGetLength(data));
}

Result<std::string> CredentialManager::get_impl(const std::string& key) {
    CFStringRef cf_service = cf_str("tccp");
    CFStringRef cf_account = cf_str(key);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);

    CFRelease(query);
    CFRelease(cf_service);
    CFRelease(cf_account);

    if (status == errSecItemNotFound) {
        return Result<std::string>::Err("Credential not found");
    }
    if (status != errSecSuccess) {
        return Result<std::string>::Err("Keychain error");
    }

    std::string value = cf_data_to_string(static_cast<CFDataRef>(result));
    CFRelease(result);
    return Result<std::string>::Ok(value);
}

Result<void> CredentialManager::set_impl(const std::string& key, const std::string& value) {
    CFStringRef cf_service = cf_str("tccp");
    CFStringRef cf_account = cf_str(key);
    CFDataRef cf_password = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.c_str()),
        value.length());

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);

    CFMutableDictionaryRef update = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, cf_password);

    OSStatus status = SecItemUpdate(query, update);

    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, cf_password);
        status = SecItemAdd(query, nullptr);
    }

    CFRelease(query);
    CFRelease(update);
    CFRelease(cf_password);
    CFRelease(cf_service);
    CFRelease(cf_account);

    if (status != errSecSuccess) {
        return Result<void>::Err("Failed to store credential in Keychain");
    }
    return Result<void>::Ok();
}

Result<void> CredentialManager::remove_impl(const std::string& key) {
    CFStringRef cf_service = cf_str("tccp");
    CFStringRef cf_account = cf_str(key);

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);

    OSStatus status = SecItemDelete(query);

    CFRelease(query);
    CFRelease(cf_service);
    CFRelease(cf_account);

    if (status != errSecSuccess) {
        return Result<void>::Err("Credential not found");
    }
    return Result<void>::Ok();
}
