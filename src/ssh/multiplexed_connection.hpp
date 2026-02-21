#pragma once

#include "connection.hpp"

class SessionMultiplexer;

// SSHConnection that executes commands through a multiplexed channel.
// Drop-in replacement for the old SSHConnection — all existing dtn_.run()
// callers work unchanged.
class MultiplexedConnection : public SSHConnection {
public:
    MultiplexedConnection(SessionMultiplexer& mux, int channel_id,
                          LIBSSH2_SESSION* session,
                          std::shared_ptr<std::mutex> io_mutex, int sock);

    SSHResult run(const std::string& cmd, int timeout_secs = 0) override;

    int channel_id() const { return channel_id_; }
    SessionMultiplexer& multiplexer() { return mux_; }

private:
    SessionMultiplexer& mux_;
    int channel_id_;
};
