#include "session_multiplexer.hpp"
#include "marker_protocol.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <libssh2.h>
#include <fmt/format.h>
#include <chrono>
#include <algorithm>



// ── Lifecycle ──────────────────────────────────────────────────

SessionMultiplexer::SessionMultiplexer(LIBSSH2_CHANNEL* channel,
                                       LIBSSH2_SESSION* session,
                                       std::shared_ptr<std::mutex> io_mutex,
                                       int sock)
    : channel_(channel), session_(session),
      io_mutex_(std::move(io_mutex)), sock_(sock) {}

SessionMultiplexer::~SessionMultiplexer() {
    stop();
}

SSHResult SessionMultiplexer::start() {
    if (running_) return SSHResult{0, "", ""};

    // Drain any stale data from the channel before starting tmux
    {
        char drain[SSH_DRAIN_BUF_SIZE];
        std::lock_guard<std::mutex> lock(*io_mutex_);
        while (libssh2_channel_read(channel_, drain, sizeof(drain)) > 0) {}
    }

    // Step 1: Kill any stale tmux session from a previous run.
    // This is a normal shell command — wait for it to complete via marker.
    send_raw("tmux kill-session -t tccp_mux 2>/dev/null\n");
    {
        // Drain the kill command output (echo + result + prompt)
        char drain[SSH_DRAIN_BUF_SIZE];
        auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < drain_deadline) {
            int n;
            {
                std::lock_guard<std::mutex> lock(*io_mutex_);
                n = libssh2_channel_read(channel_, drain, sizeof(drain));
            }
            if (n <= 0) {
                platform::sleep_ms(50);
                // Try one more read to see if prompt arrived
                std::lock_guard<std::mutex> lock(*io_mutex_);
                n = libssh2_channel_read(channel_, drain, sizeof(drain));
                if (n <= 0) break;
            }
        }
    }

    // Step 2: Start tmux in control mode (-C, single C for plain protocol).
    // -CC (double C) adds DCS passthrough wrapping for iTerm2 — wrong for
    // raw libssh2 channels where DCS bytes corrupt the protocol stream.
    if (!send_raw("tmux -C new-session -s tccp_mux\n")) {
        return SSHResult{-1, "", "Failed to send tmux start command"};
    }

    // Wait for the tmux control mode handshake.
    // tmux -C new-session outputs notifications (NOT %begin/%end):
    //   %window-add @N
    //   %sessions-changed
    //   %session-changed $N tccp_mux
    //   %layout-change @N ...
    //   %output %P <shell prompt>
    // The first %output line means the initial pane has a shell ready.
    // We parse the pane ID from %output to know our master channel.
    std::string handshake;
    std::string initial_pane_id;
    char buf[SSH_READ_BUF_SIZE];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() < deadline) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(channel_, buf, sizeof(buf) - 1);
        }
        if (n > 0) {
            handshake.append(buf, n);
            // Look for first %output line — means the pane shell is ready.
            // Format: "%output %<pane_id> <data>"
            if (initial_pane_id.empty()) {
                auto opos = handshake.find("%output %");
                if (opos != std::string::npos) {
                    auto pane_start = opos + 8;  // skip "%output "
                    auto space = handshake.find(' ', pane_start);
                    if (space != std::string::npos) {
                        initial_pane_id = handshake.substr(pane_start, space - pane_start);
                    }
                }
            }
            if (!initial_pane_id.empty()) break;
        } else if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
            platform::sleep_ms(10);
        } else {
            return SSHResult{-1, "", "Channel read error during tmux handshake"};
        }
    }

    if (initial_pane_id.empty()) {
        // Strip non-printable chars for error message
        std::string cleaned;
        for (char c : handshake) {
            if (c >= 32 || c == '\n' || c == '\r' || c == '\t')
                cleaned += c;
            else
                cleaned += fmt::format("\\x{:02x}", static_cast<unsigned char>(c));
        }
        if (cleaned.size() > 300)
            cleaned = cleaned.substr(0, 300) + "...";
        return SSHResult{-1, cleaned, "tmux control mode handshake failed (no %output received)"};
    }

    running_ = true;

    // Register the initial pane as the master channel (pane ID from handshake)
    register_master_channel(initial_pane_id);

    // Start the reader thread
    reader_thread_ = std::thread(&SessionMultiplexer::reader_loop, this);

    return SSHResult{0, "", ""};
}

void SessionMultiplexer::stop() {
    if (!running_) return;
    running_ = false;

    // Wake any threads blocked in run() waiting for output
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto& [id, state] : channels_) {
            std::lock_guard<std::mutex> olock(state->output_mutex);
            state->output_cv.notify_all();
        }
    }

    // Kill the tmux session
    send_raw("kill-server\n");

    if (reader_thread_.joinable())
        reader_thread_.join();

    // Clear channel state
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.clear();
        pane_to_channel_.clear();
    }
}

bool SessionMultiplexer::is_running() const {
    return running_;
}

// ── Channel lifecycle ──────────────────────────────────────────

void SessionMultiplexer::register_master_channel(const std::string& pane_id) {
    auto state = std::make_shared<ChannelState>();
    state->id = kMasterChannel;
    state->tmux_pane_id = pane_id;
    state->cmd_mutex = std::make_shared<std::mutex>();

    std::lock_guard<std::mutex> lock(channels_mutex_);
    channels_[kMasterChannel] = state;
    pane_to_channel_[pane_id] = kMasterChannel;
    next_channel_id_ = 1;
}

int SessionMultiplexer::open_channel() {
    if (!running_) return -1;

    // Create a new tmux window
    auto resp = send_tmux_command("new-window -P -F '#{pane_id}'");
    if (!resp.success) return -1;

    // Parse pane ID from response (e.g. "%1\n")
    std::string pane_id = resp.output;
    // Trim whitespace
    while (!pane_id.empty() && (pane_id.back() == '\n' || pane_id.back() == '\r'
                               || pane_id.back() == ' '))
        pane_id.pop_back();
    while (!pane_id.empty() && (pane_id.front() == '\n' || pane_id.front() == '\r'
                               || pane_id.front() == ' '))
        pane_id.erase(pane_id.begin());

    if (pane_id.empty() || pane_id[0] != '%') return -1;

    auto state = std::make_shared<ChannelState>();
    int id;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        id = next_channel_id_++;
        state->id = id;
        state->tmux_pane_id = pane_id;
        state->cmd_mutex = std::make_shared<std::mutex>();
        channels_[id] = state;
        pane_to_channel_[pane_id] = id;
    }

    return id;
}

void SessionMultiplexer::close_channel(int id) {
    if (id == kMasterChannel) return;  // Don't close master

    std::shared_ptr<ChannelState> state;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(id);
        if (it == channels_.end()) return;
        state = it->second;
        pane_to_channel_.erase(state->tmux_pane_id);
        channels_.erase(it);
    }

    // Kill the tmux window for this pane
    send_tmux_command("kill-pane -t " + state->tmux_pane_id);
}

// ── Programmatic command execution ─────────────────────────────

SSHResult SessionMultiplexer::run(int channel_id, const std::string& cmd,
                                  int timeout_secs) {
    std::shared_ptr<ChannelState> state;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end())
            return SSHResult{-1, "", "Invalid channel ID"};
        state = it->second;
    }

    // Per-channel serialization
    std::lock_guard<std::mutex> cmd_lock(*state->cmd_mutex);

    // Clear any pending output
    {
        std::lock_guard<std::mutex> lock(state->output_mutex);
        state->pending_output.clear();
        state->output_ready = false;
    }

    // Build marker-wrapped command and type it into the pane.
    // type_into_pane() handles newlines, control chars, and tmux escaping.
    std::string marker_cmd = build_marker_command(cmd);

    {
        std::lock_guard<std::mutex> plock(protocol_mutex_);
        type_into_pane(state->tmux_pane_id, marker_cmd.data(), marker_cmd.size());
    }

    // Wait for DONE marker in output
    int effective_timeout = (timeout_secs > 0) ? timeout_secs : SSH_CMD_TIMEOUT_SECS;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(effective_timeout);

    while (running_ && std::chrono::steady_clock::now() < deadline) {
        std::string accumulated;
        {
            std::unique_lock<std::mutex> lock(state->output_mutex);
            state->output_cv.wait_for(lock, std::chrono::milliseconds(100));
            accumulated = state->pending_output;
        }

        auto parsed = parse_marker_output(accumulated);
        if (parsed.found) {
            // Clear pending output
            std::lock_guard<std::mutex> lock(state->output_mutex);
            state->pending_output.clear();
            return SSHResult{parsed.exit_code, parsed.output, ""};
        }
    }

    if (!running_) {
        return SSHResult{-1, "", "Multiplexer stopped"};
    }

    std::string partial;
    {
        std::lock_guard<std::mutex> lock(state->output_mutex);
        partial = state->pending_output;
        state->pending_output.clear();
    }
    return SSHResult{-1, partial, "Command timed out after " +
                     std::to_string(effective_timeout) + "s"};
}

// ── Interactive I/O ────────────────────────────────────────────

void SessionMultiplexer::send_input(int channel_id, const char* data, int len) {
    if (!running_ || len <= 0) return;

    std::shared_ptr<ChannelState> state;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return;
        state = it->second;
    }

    std::lock_guard<std::mutex> plock(protocol_mutex_);
    type_into_pane(state->tmux_pane_id, data, static_cast<size_t>(len));
}

void SessionMultiplexer::send_special_key(int channel_id, const std::string& key) {
    if (!running_) return;

    std::shared_ptr<ChannelState> state;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return;
        state = it->second;
    }

    std::lock_guard<std::mutex> plock(protocol_mutex_);
    std::string cmd = fmt::format("send-keys -t {} {}", state->tmux_pane_id, key);
    send_raw(cmd + "\n");
}

void SessionMultiplexer::set_output_callback(int channel_id, OutputCallback cb) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return;
    std::lock_guard<std::mutex> cb_lock(it->second->cb_mutex);
    it->second->output_cb = std::move(cb);
}

void SessionMultiplexer::clear_output_callback(int channel_id) {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) return;
    std::lock_guard<std::mutex> cb_lock(it->second->cb_mutex);
    it->second->output_cb = nullptr;
}

void SessionMultiplexer::resize(int channel_id, int cols, int rows) {
    if (!running_) return;

    std::shared_ptr<ChannelState> state;
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(channel_id);
        if (it == channels_.end()) return;
        state = it->second;
    }

    std::lock_guard<std::mutex> plock(protocol_mutex_);
    std::string cmd = fmt::format("resize-pane -t {} -x {} -y {}",
                                  state->tmux_pane_id, cols, rows);
    send_raw(cmd + "\n");
}

// ── Reader thread ──────────────────────────────────────────────

// Unescape tmux control mode output encoding.
// tmux encodes special chars as octal \NNN.
static std::string unescape_tmux(const std::string& data) {
    std::string out;
    out.reserve(data.size());
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == '\\' && i + 3 < data.size() &&
            data[i+1] >= '0' && data[i+1] <= '3' &&
            data[i+2] >= '0' && data[i+2] <= '7' &&
            data[i+3] >= '0' && data[i+3] <= '7') {
            char c = static_cast<char>(
                (data[i+1] - '0') * 64 +
                (data[i+2] - '0') * 8 +
                (data[i+3] - '0'));
            out += c;
            i += 3;
        } else {
            out += data[i];
        }
    }
    return out;
}

void SessionMultiplexer::reader_loop() {
    char buf[SSH_READ_BUF_SIZE];
    std::string line_buf;

    // State for accumulating %begin/%end command responses
    bool in_cmd_response = false;
    std::string cmd_output;

    while (running_) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(channel_, buf, sizeof(buf) - 1);
        }

        if (n > 0) {
            line_buf.append(buf, n);

            // Process complete lines
            size_t pos = 0;
            while (pos < line_buf.size()) {
                auto nl = line_buf.find('\n', pos);
                if (nl == std::string::npos) break;

                std::string line = line_buf.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pos = nl + 1;

                // ── %begin: start accumulating command response ──
                if (line.substr(0, 6) == "%begin") {
                    in_cmd_response = true;
                    cmd_output.clear();
                    continue;
                }

                // ── %end: deliver accumulated response ──
                if (line.substr(0, 4) == "%end" && in_cmd_response) {
                    in_cmd_response = false;
                    // Trim trailing newline
                    if (!cmd_output.empty() && cmd_output.back() == '\n')
                        cmd_output.pop_back();
                    {
                        std::lock_guard<std::mutex> lock(cmd_response_mutex_);
                        cmd_responses_.push_back({true, cmd_output});
                        cmd_response_ready_ = true;
                        cmd_response_cv_.notify_one();
                    }
                    cmd_output.clear();
                    continue;
                }

                // ── %error: deliver error response ──
                if (line.substr(0, 6) == "%error" && in_cmd_response) {
                    in_cmd_response = false;
                    {
                        std::lock_guard<std::mutex> lock(cmd_response_mutex_);
                        cmd_responses_.push_back({false, cmd_output});
                        cmd_response_ready_ = true;
                        cmd_response_cv_.notify_one();
                    }
                    cmd_output.clear();
                    continue;
                }

                // ── Inside a command response: accumulate output ──
                if (in_cmd_response) {
                    cmd_output += line + "\n";
                    continue;
                }

                // ── %output: route pane output to channel ──
                if (line.size() > 8 && line.substr(0, 8) == "%output ") {
                    auto space1 = line.find(' ', 8);
                    if (space1 != std::string::npos) {
                        std::string pane_id = line.substr(8, space1 - 8);
                        std::string data = line.substr(space1 + 1);
                        std::string unescaped = unescape_tmux(data);

                        std::shared_ptr<ChannelState> state;
                        {
                            std::lock_guard<std::mutex> lock(channels_mutex_);
                            auto it = pane_to_channel_.find(pane_id);
                            if (it != pane_to_channel_.end()) {
                                auto ch_it = channels_.find(it->second);
                                if (ch_it != channels_.end())
                                    state = ch_it->second;
                            }
                        }

                        if (state) {
                            {
                                std::lock_guard<std::mutex> lock(state->output_mutex);
                                state->pending_output += unescaped;
                                state->output_cv.notify_one();
                            }

                            OutputCallback cb;
                            {
                                std::lock_guard<std::mutex> lock(state->cb_mutex);
                                cb = state->output_cb;
                            }
                            if (cb) cb(unescaped);
                        }
                    }
                }
                // Other % lines (%session-changed, %window-add, etc.) — skip
            }

            // Keep incomplete line
            if (pos < line_buf.size()) {
                line_buf = line_buf.substr(pos);
            } else {
                line_buf.clear();
            }
        } else if (n == LIBSSH2_ERROR_EAGAIN || n == 0) {
            platform::sleep_ms(5);
        } else {
            running_ = false;
            break;
        }
    }
}

// ── Internal helpers ───────────────────────────────────────────

bool SessionMultiplexer::send_raw(const std::string& text) {
    int total = static_cast<int>(text.size());
    int sent = 0;
    int retries = 0;

    while (sent < total) {
        int w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            w = libssh2_channel_write(channel_, text.c_str() + sent, total - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++retries > 100) return false;
            platform::sleep_ms(10);
            continue;
        }
        if (w < 0) return false;
        retries = 0;
        sent += w;
    }
    return true;
}

SessionMultiplexer::TmuxResponse
SessionMultiplexer::send_tmux_command(const std::string& cmd) {
    std::lock_guard<std::mutex> plock(protocol_mutex_);

    // Clear any stale responses
    {
        std::lock_guard<std::mutex> lock(cmd_response_mutex_);
        cmd_responses_.clear();
        cmd_response_ready_ = false;
    }

    // Send the command — the reader thread will parse %begin/%end
    if (!send_raw(cmd + "\n")) return {false, ""};

    // Wait for the reader thread to deliver the response
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::unique_lock<std::mutex> lock(cmd_response_mutex_);
    while (running_ && !cmd_response_ready_) {
        if (cmd_response_cv_.wait_until(lock, deadline) == std::cv_status::timeout)
            return {false, ""};
    }

    if (cmd_responses_.empty()) return {false, ""};

    auto resp = cmd_responses_.front();
    cmd_responses_.clear();
    cmd_response_ready_ = false;
    return {resp.success, resp.output};
}

std::string SessionMultiplexer::escape_for_send_keys(const std::string& text) {
    // tmux has its own command parser (NOT bash). Single-quoted strings in tmux
    // cannot contain single quotes at all — the bash '\'' idiom doesn't work.
    // Use double quotes instead: tmux supports \" \\ \$ escaping inside them.
    std::string escaped;
    escaped.reserve(text.size() + 10);
    escaped += '"';
    for (char c : text) {
        if (c == '"' || c == '\\' || c == '$' || c == '#') {
            escaped += '\\';
        }
        escaped += c;
    }
    escaped += '"';
    return escaped;
}

void SessionMultiplexer::type_into_pane(const std::string& pane_id,
                                         const char* data, size_t len) {
    // Central method for typing text into a tmux pane via send-keys.
    // Handles all tmux control mode constraints:
    //   - Newlines (\r, \n) → send-keys Enter  (raw newlines break control mode)
    //   - Control chars      → send-keys <key>  (can't be sent via -l)
    //   - Escape sequences   → send-keys <key>  (arrow keys, etc.)
    //   - Printable text     → send-keys -l "…"  (batched, double-quote escaped)
    //
    // CALLER MUST HOLD protocol_mutex_.

    auto flush_text = [&](const std::string& text) {
        if (text.empty()) return;
        std::string escaped = escape_for_send_keys(text);
        send_raw(fmt::format("send-keys -t {} -l {}\n", pane_id, escaped));
    };

    auto send_key = [&](const std::string& key) {
        send_raw(fmt::format("send-keys -t {} {}\n", pane_id, key));
    };

    std::string pending;
    size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(data[i]);

        // Escape sequences (arrow keys, Home, End, Delete, PgUp, PgDn)
        if (c == 0x1b && i + 1 < len) {
            flush_text(pending);
            pending.clear();

            if (data[i + 1] == '[' && i + 2 < len) {
                char seq = data[i + 2];
                if (seq == 'A') { send_key("Up");    i += 3; continue; }
                if (seq == 'B') { send_key("Down");  i += 3; continue; }
                if (seq == 'C') { send_key("Right"); i += 3; continue; }
                if (seq == 'D') { send_key("Left");  i += 3; continue; }
                if (seq == 'H') { send_key("Home");  i += 3; continue; }
                if (seq == 'F') { send_key("End");   i += 3; continue; }
                if (seq >= '0' && seq <= '9' && i + 3 < len && data[i + 3] == '~') {
                    if (seq == '3') { send_key("DC");       i += 4; continue; }
                    if (seq == '2') { send_key("IC");       i += 4; continue; }
                    if (seq == '5') { send_key("PageUp");   i += 4; continue; }
                    if (seq == '6') { send_key("PageDown"); i += 4; continue; }
                    send_key("Escape"); i += 1; continue;
                }
            }
            if (data[i + 1] == 'O' && i + 2 < len) {
                char seq = data[i + 2];
                if (seq == 'A') { send_key("Up");    i += 3; continue; }
                if (seq == 'B') { send_key("Down");  i += 3; continue; }
                if (seq == 'C') { send_key("Right"); i += 3; continue; }
                if (seq == 'D') { send_key("Left");  i += 3; continue; }
                if (seq == 'H') { send_key("Home");  i += 3; continue; }
                if (seq == 'F') { send_key("End");   i += 3; continue; }
            }
            send_key("Escape"); i += 1; continue;
        }

        // Newlines → Enter (raw \r or \n breaks tmux control mode commands)
        if (c == '\r' || c == '\n') {
            flush_text(pending);
            pending.clear();
            send_key("Enter");
            i++;
            if (c == '\r' && i < len && data[i] == '\n') i++;  // collapse \r\n
            continue;
        }

        // Backspace
        if (c == 0x7f || c == 0x08) {
            flush_text(pending); pending.clear();
            send_key("BSpace"); i++; continue;
        }

        // Tab
        if (c == '\t') {
            flush_text(pending); pending.clear();
            send_key("Tab"); i++; continue;
        }

        // Ctrl+A through Ctrl+Z (except Tab=0x09, LF=0x0a, CR=0x0d — handled above)
        if (c >= 0x01 && c <= 0x1a) {
            flush_text(pending); pending.clear();
            send_key(fmt::format("C-{}", static_cast<char>('a' + c - 1)));
            i++; continue;
        }

        // Printable — accumulate for batch send
        pending += static_cast<char>(c);
        i++;
    }
    flush_text(pending);
}
