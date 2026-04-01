#pragma once

#include "connection.hpp"
#include "compute_hop.hpp"  // escape_for_ssh
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
};
