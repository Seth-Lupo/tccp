#pragma once

#include <string>
#include <memory>
#include <functional>
#include <core/types.hpp>
#include "expect.hpp"

// libssh2 forward declaration
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

struct AuthContext {
    LIBSSH2_CHANNEL* channel;
    std::string user;
    std::string password;
    StatusCallback status;
};

class Authenticator {
public:
    virtual ~Authenticator() = default;
    virtual SSHResult authenticate(AuthContext& ctx) = 0;
};

class PasswordAuthenticator : public Authenticator {
public:
    SSHResult authenticate(AuthContext& ctx) override;

private:
    ExpectMatcher matcher_;
};

class KeyAuthenticator : public Authenticator {
public:
    SSHResult authenticate(AuthContext& ctx) override;

private:
    ExpectMatcher matcher_;
};

class DuoAuthenticator : public Authenticator {
public:
    DuoAuthenticator(bool auto_duo = false) : auto_duo_(auto_duo) {}
    SSHResult authenticate(AuthContext& ctx) override;

private:
    ExpectMatcher matcher_;
    bool auto_duo_;
};

// Helper to send command and wait for response
SSHResult send_command(LIBSSH2_CHANNEL* channel,
                       const std::string& command,
                       const std::vector<std::string>& expected_prompts);
