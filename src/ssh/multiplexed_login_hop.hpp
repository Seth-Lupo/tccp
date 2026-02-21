#pragma once

#include "multiplexed_connection.hpp"
#include <string>

// Login node commands via SSH hop, through a multiplexed channel.
// Wraps commands in SSH hop automatically, just like LoginHopConnection.
class MultiplexedLoginHop : public MultiplexedConnection {
public:
    MultiplexedLoginHop(SessionMultiplexer& mux, int channel_id,
                        LIBSSH2_SESSION* session,
                        std::shared_ptr<std::mutex> io_mutex, int sock,
                        std::string login_host);

    SSHResult run(const std::string& cmd, int timeout_secs = 0) override;

private:
    std::string login_host_;

    static std::string escape_for_ssh(const std::string& cmd);
};
