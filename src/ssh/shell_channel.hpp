#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <core/types.hpp>
#include "result.hpp"

// libssh2 forward declarations
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

// RAII handle for a dynamically-created shell channel.
// Owns the channel and closes+frees on destruction.
// Has its own cmd_mutex_ to serialize command sequences.
// All libssh2 calls are protected by brief io_mutex_ holds.
class ShellChannel {
public:
    ShellChannel(LIBSSH2_CHANNEL* ch, LIBSSH2_SESSION* session,
                 std::shared_ptr<std::mutex> io_mutex, int sock);
    ~ShellChannel();

    // Run a command with done-marker protocol (same as SSHConnection::run).
    // Acquires cmd_mutex_ for the duration. Brief io_mutex_ holds per API call.
    SSHResult run(const std::string& command, int timeout_secs = 0);

    LIBSSH2_CHANNEL* raw() const { return ch_; }
    int socket() const { return sock_; }
    std::shared_ptr<std::mutex> io_mutex() const { return io_mutex_; }

    // Non-copyable, movable
    ShellChannel(const ShellChannel&) = delete;
    ShellChannel& operator=(const ShellChannel&) = delete;
    ShellChannel(ShellChannel&& other) noexcept;
    ShellChannel& operator=(ShellChannel&& other) noexcept;

private:
    LIBSSH2_CHANNEL* ch_;
    LIBSSH2_SESSION* session_;
    std::shared_ptr<std::mutex> io_mutex_;
    int sock_;
    std::mutex cmd_mutex_;

    void close_channel();
};
