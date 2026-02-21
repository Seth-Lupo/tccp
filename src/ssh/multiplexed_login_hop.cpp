#include "multiplexed_login_hop.hpp"
#include "session_multiplexer.hpp"

MultiplexedLoginHop::MultiplexedLoginHop(SessionMultiplexer& mux,
                                         int channel_id,
                                         LIBSSH2_SESSION* session,
                                         std::shared_ptr<std::mutex> io_mutex,
                                         int sock,
                                         std::string login_host)
    : MultiplexedConnection(mux, channel_id, session, std::move(io_mutex), sock),
      login_host_(std::move(login_host)) {}

SSHResult MultiplexedLoginHop::run(const std::string& cmd, int timeout_secs) {
    // Wrap command in SSH hop to login node, same as LoginHopConnection.
    // </dev/null prevents SSH from consuming the next line in the TTY buffer.
    std::string wrapped = "ssh -T -o StrictHostKeyChecking=no -o LogLevel=ERROR " +
                          login_host_ + " " + escape_for_ssh(cmd) + " </dev/null";
    return MultiplexedConnection::run(wrapped, timeout_secs);
}

std::string MultiplexedLoginHop::escape_for_ssh(const std::string& cmd) {
    std::string escaped;
    escaped.reserve(cmd.size() + 10);
    escaped += '\'';
    for (char c : cmd) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += '\'';
    return escaped;
}
