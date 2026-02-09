#include "connection.hpp"
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

SSHResult SSHConnection::run(const std::string& command) {
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Drain any stale data sitting in the channel buffer
    char drain[4096];
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
    while (sent < total) {
        int w = libssh2_channel_write(channel_, full_cmd.c_str() + sent, total - sent);
        if (w == LIBSSH2_ERROR_EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (w < 0) {
            return SSHResult{-1, "", "Failed to send command"};
        }
        sent += w;
    }

    // Read until we see the marker
    std::string output;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);

    while (std::chrono::steady_clock::now() < deadline) {
        int n = libssh2_channel_read(channel_, buf, sizeof(buf) - 1);
        if (n > 0) {
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
                        exit_code = std::stoi(code_str.substr(num_start));
                    }
                }

                // Extract text between BEGIN and DONE markers.
                // The BEGIN marker line (including its trailing newline)
                // cleanly separates the command echo from real output.
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            break;
        }
    }

    // Timeout or error — return what we have
    return SSHResult{-1, output, "Command timed out or channel error"};
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

    // Build heredoc command
    std::string cmd = "cat > " + remote + " << 'TCCP_EOF'\n" + content + "\nTCCP_EOF";

    return run(cmd);
}

SSHResult SSHConnection::download(const std::string& remote, const fs::path& local) {
    if (!channel_) {
        return SSHResult{-1, "", "No channel available"};
    }

    // Use cat to read file
    std::string cmd = "cat " + remote;
    auto result = run(cmd);

    if (result.failed()) {
        return result;
    }

    // Write to local file
    try {
        fs::create_directories(local.parent_path());
        std::ofstream file(local, std::ios::binary);
        file << result.stdout_data;
        file.close();
        return SSHResult{0, "Downloaded to " + local.string(), ""};
    } catch (const std::exception& e) {
        return SSHResult{-1, "", "Failed to write file: " + std::string(e.what())};
    }
}
