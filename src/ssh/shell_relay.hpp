#pragma once

#include <memory>
#include <mutex>
#include <string>

class ConnectionFactory;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

// ShellRelay: run commands on the primary shell channel.
//
// The primary shell channel is the ONLY channel with Kerberos tickets,
// making it the sole safe path for SSH hops to login/compute nodes.
// All operations are serialized via the session mutex.
//
// Protocol: send command → skip echoed command line → relay output →
//           detect done marker → drain. On early exit, Ctrl+C kills
//           the running command before draining.
//
// Simple usage (raw stdin/stdout relay):
//     ShellRelay relay(mux);
//     relay.run("ssh -t login 'bash'");
//
// Advanced usage (custom relay loop):
//     ShellRelay relay(mux);
//     if (!relay.start("ssh -t compute '...'")) return;
//     while (!relay.done()) {
//         int n = libssh2_channel_read(relay.channel(), buf, sizeof(buf));
//         std::string text = relay.feed(buf, n);
//         // custom output processing...
//     }
//     relay.stop();

class ShellRelay {
public:
    explicit ShellRelay(ConnectionFactory& factory);
    ~ShellRelay();

    // ── Simple interface ────────────────────────────────────────

    // Run command with raw stdin/stdout relay. Blocks until done.
    // Handles raw mode, resize propagation, and cleanup.
    void run(const std::string& command);

    // ── Advanced lifecycle ──────────────────────────────────────

    // Lock session, drain stale data, send command with done marker.
    // Returns false if channel unavailable.
    bool start(const std::string& command);

    // Feed raw channel output through the protocol processor.
    // Skips echoed command, detects done marker, buffers partial markers.
    // Returns text safe to display. Check done() afterward.
    std::string feed(const char* data, int len);

    // True once the done marker has been detected.
    bool done() const;

    // Clean up. Sends Ctrl+C if command still running. Drains channel.
    // Releases session lock. Safe to call multiple times.
    void stop();

    // ── Raw access ─────────────────────────────────────────────

    LIBSSH2_CHANNEL* channel() const;
    int socket() const;

private:
    ConnectionFactory& factory_;
    LIBSSH2_CHANNEL* ch_ = nullptr;
    std::unique_lock<std::recursive_mutex> lock_;
    bool started_ = false;
    bool skip_echo_ = true;
    bool done_ = false;
    std::string buf_;

    void drain();
    void send(const std::string& text);
};
