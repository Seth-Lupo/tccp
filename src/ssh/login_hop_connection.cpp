#include "login_hop_connection.hpp"

LoginHopConnection::LoginHopConnection(LIBSSH2_CHANNEL* channel,
                                       LIBSSH2_SESSION* session,
                                       std::shared_ptr<std::recursive_mutex> mutex,
                                       std::string login_host)
    : SSHConnection(channel, session, std::move(mutex)),
      login_host_(std::move(login_host)) {}

SSHResult LoginHopConnection::run(const std::string& command, int timeout_secs) {
    // Wrap command in SSH hop to login node via DTN shell.
    // </dev/null prevents SSH from consuming the next line in the TTY buffer
    // (the DONE marker), which SSHConnection::run() sends as a separate line.
    std::string wrapped = "ssh -T -o StrictHostKeyChecking=no " +
                          login_host_ + " " + escape_for_ssh(command) + " </dev/null";
    return SSHConnection::run(wrapped, timeout_secs);
}

std::string LoginHopConnection::escape_for_ssh(const std::string& cmd) {
    // Single-quote wrapping with ' → '\'' replacement
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
