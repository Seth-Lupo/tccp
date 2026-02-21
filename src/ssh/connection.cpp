#include "connection.hpp"
#include "marker_protocol.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <libssh2.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>

SSHConnection::SSHConnection(LIBSSH2_CHANNEL* channel, LIBSSH2_SESSION* session,
                             std::shared_ptr<std::mutex> io_mutex, int sock,
                             std::shared_ptr<std::mutex> cmd_mutex)
    : channel_(channel), session_(session), io_mutex_(io_mutex),
      sock_(sock), cmd_mutex_(cmd_mutex) {
}

bool SSHConnection::is_active() const {
    return channel_ != nullptr;
}

SSHResult SSHConnection::run(const std::string& command, int timeout_secs) {
    std::lock_guard<std::mutex> cmd_lock(*cmd_mutex_);
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Drain any stale data sitting in the channel buffer
    char drain[SSH_DRAIN_BUF_SIZE];
    {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        while (libssh2_channel_read(channel_, drain, sizeof(drain)) > 0) {}
    }

    // Send command wrapped in BEGIN/DONE markers.
    std::string full_cmd = build_marker_command(command);

    int total = full_cmd.length();
    int sent = 0;
    int write_retries = 0;
    while (sent < total) {
        int w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            w = libssh2_channel_write(channel_, full_cmd.c_str() + sent, total - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++write_retries > 100) {
                return SSHResult{-1, "", "Write stalled (EAGAIN for too long)"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (w < 0) {
            return SSHResult{-1, "", "Failed to send command (channel write error)"};
        }
        write_retries = 0;
        sent += w;
    }

    // Read until we see the marker
    std::string output;
    char buf[SSH_READ_BUF_SIZE];
    int effective_timeout = (timeout_secs > 0) ? timeout_secs : SSH_CMD_TIMEOUT_SECS;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(effective_timeout);
    int consecutive_eagain = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(channel_, buf, sizeof(buf) - 1);
        }
        if (n > 0) {
            consecutive_eagain = 0;
            output.append(buf, n);
            auto parsed = parse_marker_output(output);
            if (parsed.found) {
                return SSHResult{parsed.exit_code, parsed.output, ""};
            }
        } else if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
            // Stall detection: for default-timeout commands, bail after 5s of silence.
            // For explicit long timeouts (e.g. container pull), skip stall detection
            // and rely solely on the deadline — long silence is expected.
            if (timeout_secs == 0 && ++consecutive_eagain > 500) {
                return SSHResult{-1, output, "Channel stalled (no data for 5s)"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            // Negative return other than EAGAIN = channel error
            return SSHResult{-1, output, "SSH channel read error"};
        }
    }

    // Timeout or error — return what we have
    return SSHResult{-1, output, "Command timed out after " + std::to_string(effective_timeout) + "s"};
}

// ── Base64 decode for binary-safe transfer over PTY ──

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::string base64_decode(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 3 / 4);
    int val = 0, bits = -8;
    for (char c : input) {
        if (c == '\r' || c == '\n' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) break;  // '=' or invalid → stop
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

SSHResult SSHConnection::upload(const fs::path& local, const std::string& remote) {
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Read file content
    std::ifstream file(local, std::ios::binary);
    if (!file) {
        return SSHResult{-1, "", "Cannot read file: " + local.string()};
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

    // Base64-encode for binary safety, split into lines so the PTY doesn't
    // choke on a single giant echo.  Feed through a heredoc to base64 -d.
    std::string encoded = base64_encode(content);

    // Split into 76-char lines (standard base64 line length)
    std::string lines;
    for (size_t i = 0; i < encoded.size(); i += 76) {
        lines += encoded.substr(i, 76) + "\n";
    }

    std::string cmd = "base64 -d > " + remote + " << 'TCCP_B64_EOF'\n"
                      + lines + "TCCP_B64_EOF";

    return run(cmd);
}

SSHResult SSHConnection::run_with_input(const std::string& command,
                                        const char* data, size_t data_len,
                                        int timeout_secs) {
    if (!session_) {
        return SSHResult{-1, "", "No session available for run_with_input"};
    }

    // Open a new exec channel (no PTY — binary-clean)
    LIBSSH2_CHANNEL* exec_ch = nullptr;
    auto open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < open_deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            exec_ch = libssh2_channel_open_session(session_);
            if (!exec_ch && libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
                return SSHResult{-1, "", "Failed to open exec channel"};
            }
        }
        if (exec_ch) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!exec_ch) {
        return SSHResult{-1, "", "Timed out opening exec channel"};
    }

    // Execute the command
    int rc;
    auto exec_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < exec_deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            rc = libssh2_channel_exec(exec_ch, command.c_str());
        }
        if (rc != LIBSSH2_ERROR_EAGAIN) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (rc != 0) {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        libssh2_channel_free(exec_ch);
        return SSHResult{-1, "", "Failed to exec command on channel"};
    }

    // Write all data to stdin
    size_t sent = 0;
    int write_retries = 0;
    while (sent < data_len) {
        ssize_t w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            w = libssh2_channel_write(exec_ch, data + sent, data_len - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++write_retries > 1000) {
                std::lock_guard<std::mutex> lock(*io_mutex_);
                libssh2_channel_close(exec_ch);
                libssh2_channel_free(exec_ch);
                return SSHResult{-1, "", "Write stalled sending data to channel"};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (w < 0) {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_channel_close(exec_ch);
            libssh2_channel_free(exec_ch);
            return SSHResult{-1, "", "Channel write error sending data"};
        }
        write_retries = 0;
        sent += static_cast<size_t>(w);
    }

    // Close stdin (send EOF) so the remote command knows input is done
    {
        int eof_rc;
        do {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            eof_rc = libssh2_channel_send_eof(exec_ch);
        } while (eof_rc == LIBSSH2_ERROR_EAGAIN &&
                 (std::this_thread::sleep_for(std::chrono::milliseconds(10)), true));
    }

    // Read stdout until channel closes
    std::string output;
    char buf[SSH_READ_BUF_SIZE];
    int effective_timeout = (timeout_secs > 0) ? timeout_secs : SSH_CMD_TIMEOUT_SECS;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(effective_timeout);

    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(exec_ch, buf, sizeof(buf) - 1);
        }
        if (n > 0) {
            output.append(buf, static_cast<size_t>(n));
        } else if (n == 0 || n == LIBSSH2_ERROR_EAGAIN) {
            bool eof;
            {
                std::lock_guard<std::mutex> lock(*io_mutex_);
                eof = libssh2_channel_eof(exec_ch);
            }
            if (eof) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            break;  // read error
        }
    }

    // Read stderr too (for error diagnostics)
    std::string stderr_data;
    while (true) {
        ssize_t n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read_stderr(exec_ch, buf, sizeof(buf) - 1);
        }
        if (n > 0) {
            stderr_data.append(buf, static_cast<size_t>(n));
        } else {
            break;
        }
    }

    // Get exit status
    int exit_status = 0;
    {
        do {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            rc = libssh2_channel_close(exec_ch);
        } while (rc == LIBSSH2_ERROR_EAGAIN &&
                 (std::this_thread::sleep_for(std::chrono::milliseconds(10)), true));
    }
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        exit_status = libssh2_channel_get_exit_status(exec_ch);
    }

    {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        libssh2_channel_free(exec_ch);
    }

    return SSHResult{exit_status, output, stderr_data};
}

SSHResult SSHConnection::download(const std::string& remote, const fs::path& local) {
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Base64-encode on remote to avoid PTY \n → \r\n corruption
    std::string cmd = "base64 " + remote;
    auto result = run(cmd);

    if (result.failed()) {
        return result;
    }

    // Decode and write to local file
    try {
        fs::create_directories(local.parent_path());
        std::string decoded = base64_decode(result.stdout_data);
        std::ofstream file(local, std::ios::binary);
        file.write(decoded.data(), decoded.size());
        file.close();
        return SSHResult{0, "Downloaded to " + local.string(), ""};
    } catch (const std::exception& e) {
        return SSHResult{-1, "", "Failed to write file: " + std::string(e.what())};
    }
}
