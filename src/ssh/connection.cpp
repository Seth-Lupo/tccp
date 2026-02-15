#include "connection.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <libssh2.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>

SSHConnection::SSHConnection(LIBSSH2_CHANNEL* channel)
    : channel_(channel) {
}

bool SSHConnection::is_active() const {
    return channel_ != nullptr;
}

SSHResult SSHConnection::run(const std::string& command, int timeout_secs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Drain any stale data sitting in the channel buffer
    char drain[SSH_DRAIN_BUF_SIZE];
    while (libssh2_channel_read(channel_, drain, sizeof(drain)) > 0) {}

    // Send command wrapped in BEGIN/DONE markers.
    // We use shell string concatenation (BEG''IN) so the literal marker
    // text doesn't appear in the command string the PTY echoes back.
    // The echo commands still output the full marker strings.
    std::string begin_marker = "__TCCP_BEGIN__";
    std::string done_marker  = "__TCCP_DONE__";
    std::string full_cmd = "echo __TCCP_BEG''IN__; " + command +
                           "\necho __TCCP_DO''NE__ $?\n";

    int total = full_cmd.length();
    int sent = 0;
    int write_retries = 0;
    while (sent < total) {
        int w = libssh2_channel_write(channel_, full_cmd.c_str() + sent, total - sent);
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
        int n = libssh2_channel_read(channel_, buf, sizeof(buf) - 1);
        if (n > 0) {
            consecutive_eagain = 0;
            output.append(buf, n);
            // Check if DONE marker has arrived
            auto done_pos = output.find(done_marker);
            if (done_pos != std::string::npos) {
                // Parse exit code after DONE marker (safe_stoi avoids exception)
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
                    // Skip the newline after the BEGIN marker
                    if (content_start < output.length() && output[content_start] == '\r')
                        content_start++;
                    if (content_start < output.length() && output[content_start] == '\n')
                        content_start++;
                    clean = output.substr(content_start, done_pos - content_start);
                } else {
                    // Fallback: no BEGIN marker found, take everything before DONE
                    clean = output.substr(0, done_pos);
                }

                // Trim trailing whitespace/prompt remnants
                auto last_content = clean.find_last_not_of(" \t\r\n");
                if (last_content != std::string::npos) {
                    clean = clean.substr(0, last_content + 1);
                } else {
                    clean.clear();
                }

                return SSHResult{exit_code, clean, ""};
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

// ── Base64 encode/decode for binary-safe transfer over PTY ──

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; i += 3) {
        unsigned val = data[i] << 16;
        if (i + 1 < len) val |= data[i + 1] << 8;
        if (i + 2 < len) val |= data[i + 2];
        out += B64_CHARS[(val >> 18) & 0x3F];
        out += B64_CHARS[(val >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(val >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[val & 0x3F] : '=';
    }
    return out;
}

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
