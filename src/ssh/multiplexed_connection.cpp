#include "multiplexed_connection.hpp"
#include "session_multiplexer.hpp"

MultiplexedConnection::MultiplexedConnection(SessionMultiplexer& mux,
                                             int channel_id,
                                             LIBSSH2_SESSION* session,
                                             std::shared_ptr<std::mutex> io_mutex,
                                             int sock)
    : SSHConnection(mux.raw_channel(), session, std::move(io_mutex), sock,
                    std::make_shared<std::mutex>()),  // dummy cmd_mutex (not used)
      mux_(mux), channel_id_(channel_id) {}

SSHResult MultiplexedConnection::run(const std::string& cmd, int timeout_secs) {
    return mux_.run(channel_id_, cmd, timeout_secs);
}
