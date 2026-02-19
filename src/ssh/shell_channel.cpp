#include "shell_channel.hpp"
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
    std::string begin_marker = "__TCCP_BEGIN__";
    std::string done_marker  = "__TCCP_DONE__";
    std::string full_cmd;
    if (command.find('\n') == std::string::npos) {
        full_cmd = "echo __TCCP_BEG''IN__; " + command +
                   "; echo __TCCP_DO''NE__ $?\n";
    } else {
        full_cmd = "echo __TCCP_BEG''IN__; " + command +
                   "\necho __TCCP_DO''NE__ $?\n";
    }

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
            // Check if DONE marker has arrived
            auto done_pos = output.find(done_marker);
            if (done_pos != std::string::npos) {
                // Parse exit code after DONE marker
                int exit_code = 0;
                auto code_start = done_pos + done_marker.length();
                if (code_start < output.length()) {
                    std::string code_str = output.substr(code_start);
                    auto num_start = code_str.find_first_of("0123456789");
                    if (num_start != std::string::npos) {
                        exit_code = safe_stoi(code_str.substr(num_start), 0);
                    }
                }

                // Extract text between BEGIN and DONE markers.
                std::string clean;
                auto begin_pos = output.find(begin_marker);
                if (begin_pos != std::string::npos) {
                    auto content_start = begin_pos + begin_marker.length();
                    if (content_start < output.length() && output[content_start] == '\r')
                        content_start++;
                    if (content_start < output.length() && output[content_start] == '\n')
                        content_start++;
                    clean = output.substr(content_start, done_pos - content_start);
                } else {
                    clean = output.substr(0, done_pos);
                }

                // Strip trailing whitespace
                while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r'
                                       || clean.back() == ' ' || clean.back() == '\t'))
                    clean.pop_back();

                // Strip trailing prompt echo of the sentinel command
                auto last_nl = clean.rfind('\n');
                if (last_nl != std::string::npos) {
                    auto trailing = clean.substr(last_nl + 1);
                    if (trailing.find("__TCCP_DO") != std::string::npos) {
                        clean = clean.substr(0, last_nl);
                    }
                } else if (clean.find("__TCCP_DO") != std::string::npos) {
                    clean.clear();
                }

                // Trim remaining trailing whitespace
                auto last_content = clean.find_last_not_of(" \t\r\n");
                if (last_content != std::string::npos) {
                    clean = clean.substr(0, last_content + 1);
                } else {
                    clean.clear();
                }

                return SSHResult{exit_code, clean, ""};
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
