#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <core/config.hpp>
#include "session.hpp"
#include "connection.hpp"
#include "login_hop_connection.hpp"

class MultiplexedShell;
class SessionMultiplexer;

// ConnectionFactory: single SSH session, multiplexed connections.
//
// One Duo push authenticates the session. All connections granted by this
// factory share that session — consumers don't know about multiplexing.
//
// Internally uses tmux -CC to multiplex the primary shell channel into
// independent virtual channels. Each dtn()/login() call gets its own
// multiplexed channel; interactive shells get dedicated channels that
// don't block programmatic commands.
//
// Connection types:
//   dtn()          — non-interactive commands on DTN (multiplexed channel)
//   login()        — non-interactive commands on login node (multiplexed + SSH hop)
//   shell()        — interactive relay on dedicated channel (has Kerberos)
//   exec_channel() — raw exec channel (no PTY, no Duo, no Kerberos)
//   tunnel()       — direct-tcpip channel (port forwarding)

class ConnectionFactory {
public:
    explicit ConnectionFactory(const Config& config);
    ~ConnectionFactory();

    SSHResult connect(StatusCallback callback = nullptr);
    SSHResult disconnect();
    bool is_connected() const;
    bool check_alive();
    void send_keepalive();

    // ── Connection grants ──────────────────────────────────────

    // Non-interactive command execution (multiplexed — no global blocking)
    SSHConnection& dtn();
    SSHConnection& login();

    // Interactive relay on a dedicated multiplexed channel (has Kerberos)
    MultiplexedShell shell();

    // ── Channel grants ─────────────────────────────────────────

    // New exec channel (no PTY, no Duo, no Kerberos) — caller owns result
    LIBSSH2_CHANNEL* exec_channel();

    // Direct-tcpip tunnel for port forwarding — caller owns result
    LIBSSH2_CHANNEL* tunnel(const std::string& host, int port);

    // ── Raw access ─────────────────────────────────────────────

    LIBSSH2_CHANNEL* primary_channel();
    LIBSSH2_SESSION* raw_session();
    int raw_socket();
    std::shared_ptr<std::mutex> io_mutex();
    SessionMultiplexer* multiplexer();

private:
    const Config& config_;
    std::unique_ptr<SessionManager> session_;
    std::unique_ptr<SessionMultiplexer> mux_;
    std::unique_ptr<SSHConnection> dtn_;
    std::unique_ptr<SSHConnection> login_;

    SSHResult connect_session(StatusCallback callback);
};
