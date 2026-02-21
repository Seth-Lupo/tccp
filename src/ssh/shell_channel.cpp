#include "shell_channel.hpp"
#include "marker_protocol.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#include <chrono>
#include <thread>

ShellChannel::ShellChannel(LIBSSH2_CHANNEL* ch, LIBSSH2_SESSION* session,
                           std::shared_ptr<std::mutex> io_mutex, int sock)
    : ch_(ch), session_(session), io_mutex_(std::move(io_mutex)), sock_(sock) {}

ShellChannel::~ShellChannel() {
    close_channel();
}

ShellChannel::ShellChannel(ShellChannel&& other) noexcept
    : ch_(other.ch_), session_(other.session_),
      io_mutex_(std::move(other.io_mutex_)), sock_(other.sock_) {
    other.ch_ = nullptr;
    other.session_ = nullptr;
    other.sock_ = -1;
}

ShellChannel& ShellChannel::operator=(ShellChannel&& other) noexcept {
    if (this != &other) {
        close_channel();
        ch_ = other.ch_;
        session_ = other.session_;
        io_mutex_ = std::move(other.io_mutex_);
        sock_ = other.sock_;
        other.ch_ = nullptr;
        other.session_ = nullptr;
        other.sock_ = -1;
    }
    return *this;
}

void ShellChannel::close_channel() {
    if (!ch_ || !io_mutex_) return;
    std::lock_guard<std::mutex> lock(*io_mutex_);
    libssh2_channel_close(ch_);
    libssh2_channel_free(ch_);
    ch_ = nullptr;
}

SSHResult ShellChannel::run(const std::string& command, int timeout_secs) {
    std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);

    if (!ch_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Drain any stale data sitting in the channel buffer
    {
        char drain[SSH_DRAIN_BUF_SIZE];
        std::lock_guard<std::mutex> lock(*io_mutex_);
        while (libssh2_channel_read(ch_, drain, sizeof(drain)) > 0) {}
    }

    // Send command wrapped in BEGIN/DONE markers.
    std::string full_cmd = build_marker_command(command);

    // Write command with brief io_mutex_ holds
    int total = full_cmd.length();
    int sent = 0;
    int write_retries = 0;
    while (sent < total) {
        int w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            w = libssh2_channel_write(ch_, full_cmd.c_str() + sent, total - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++write_retries > 100) {
                return SSHResult{-1, "", "Write stalled (EAGAIN for too long)"};
            }
            platform::sleep_ms(10);
            continue;
        }
        if (w < 0) {
            return SSHResult{-1, "", "Failed to send command (channel write error)"};
        }
        write_retries = 0;
        sent += w;
    }

    // Read until we see the marker — brief io_mutex_ holds per read
    std::string output;
    char buf[SSH_READ_BUF_SIZE];
    int effective_timeout = (timeout_secs > 0) ? timeout_secs : SSH_CMD_TIMEOUT_SECS;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(effective_timeout);
    int consecutive_eagain = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(ch_, buf, sizeof(buf) - 1);
        }
        if (n > 0) {
            consecutive_eagain = 0;
            output.append(buf, n);
            auto parsed = parse_marker_output(output);
            if (parsed.found) {
                return SSHResult{parsed.exit_code, parsed.output, ""};
            }
        } else if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
            if (timeout_secs == 0 && ++consecutive_eagain > 500) {
                return SSHResult{-1, output, "Channel stalled (no data for 5s)"};
            }
            // Poll socket without holding io_mutex_
            platform::poll_socket(sock_, POLLIN, 10);
        } else {
            return SSHResult{-1, output, "SSH channel read error"};
        }
    }

    return SSHResult{-1, output, "Command timed out after " + std::to_string(effective_timeout) + "s"};
}
