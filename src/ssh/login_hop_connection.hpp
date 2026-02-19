#pragma once

#include "connection.hpp"
#include <string>

class LoginHopConnection : public SSHConnection {
public:
    LoginHopConnection(LIBSSH2_CHANNEL* channel, LIBSSH2_SESSION* session,
                       std::shared_ptr<std::mutex> io_mutex, int sock,
                       std::shared_ptr<std::mutex> cmd_mutex,
                       std::string login_host);

    SSHResult run(const std::string& command, int timeout_secs = 0) override;

private:
    std::string login_host_;

    static std::string escape_for_ssh(const std::string& cmd);
};
