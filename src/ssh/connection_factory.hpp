#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <core/config.hpp>
#include "session.hpp"
#include "connection.hpp"
#include "login_hop_connection.hpp"

class ShellRelay;

// ConnectionFactory: single SSH session, multiplexed connections.
//
// One Duo push authenticates the session. All connections granted by this
// factory share that session — consumers don't know about multiplexing.
//
// Connection types:
//   dtn()          — non-interactive commands on DTN
//   login()        — non-interactive commands on login node (auto SSH hop)
//   shell()        — interactive relay on primary channel (has Kerberos)
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

    // Non-interactive command execution (serialized on primary shell channel)
    SSHConnection& dtn();
    SSHConnection& login();

    // Interactive relay on primary shell channel (has Kerberos for SSH hops)
    ShellRelay shell();

    // ── Channel grants ─────────────────────────────────────────

    // New exec channel (no PTY, no Duo, no Kerberos) — caller owns result
    LIBSSH2_CHANNEL* exec_channel();

    // Direct-tcpip tunnel for port forwarding — caller owns result
    LIBSSH2_CHANNEL* tunnel(const std::string& host, int port);

    // ── Raw access ─────────────────────────────────────────────

    LIBSSH2_CHANNEL* primary_channel();
    LIBSSH2_SESSION* raw_session();
    int raw_socket();
    std::shared_ptr<std::recursive_mutex> session_mutex();

private:
    const Config& config_;
    std::unique_ptr<SessionManager> session_;
    std::unique_ptr<SSHConnection> dtn_;
    std::unique_ptr<LoginHopConnection> login_;

    SSHResult connect_session(StatusCallback callback);
};
