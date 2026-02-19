#include "job_view.hpp"
#include "terminal_output.hpp"
#include "terminal_ui.hpp"
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <ssh/shell_relay.hpp>
#include <ssh/connection_factory.hpp>
#include <iostream>
#include <libssh2.h>
#include <fmt/format.h>
#ifndef _WIN32
#  include <unistd.h>
#  include <poll.h>
#endif

// ── Constants ──────────────────────────────────────────────────

static constexpr const char* JOB_START_MARKER = "__TCCP_JOB_START__";
static constexpr size_t JOB_START_MARKER_LEN = 18;

// ── Resize flag ─────────────────────────────────────────────────

static volatile int g_need_resize = 0;

// ── RAII cleanup guard ──────────────────────────────────────────

namespace {

struct AttachCleanup {
    platform::RawModeGuard* raw_guard = nullptr;
    bool has_resize = false;

    ~AttachCleanup() {
        delete raw_guard;
        if (has_resize)
            platform::remove_terminal_resize();
        write(STDOUT_FILENO, "\033[r", 3);
    }
};

} // namespace

// ── JobView implementation ──────────────────────────────────────

JobView::JobView(ConnectionFactory& factory, const std::string& compute_node,
                 const std::string& username, const std::string& dtach_socket,
                 const std::string& job_name, const std::string& slurm_id,
                 const std::string& job_id, const std::string& dtn_host,
                 const std::string& scratch_path, bool canceled)
    : factory_(factory), compute_node_(compute_node),
      username_(username), dtach_socket_(dtach_socket),
      job_name_(job_name), slurm_id_(slurm_id),
      job_id_(job_id), dtn_host_(dtn_host),
      scratch_path_(scratch_path), canceled_(canceled) {}

void JobView::draw_header(bool terminated, int exit_code, bool canceled) {
    TerminalUI::StatusBar bar;
    bar.job_name = job_name_;
    bar.slurm_id = slurm_id_;
    bar.node = compute_node_;

    if (terminated && canceled) {
        bar.status = "CANCELED";
        bar.l2_bg = "\033[48;2;60;60;60m";
        bar.l2_fg = "\033[38;2;120;120;120m";
    } else if (terminated) {
        bar.status = exit_code == 0 ? "TERMINATED" : fmt::format("TERMINATED (exit {})", exit_code);
        bar.l2_bg = "\033[48;2;60;60;60m";
        bar.l2_fg = "\033[38;2;120;120;120m";
    } else if (!got_first_output_) {
        bar.status = "Loading...";
        bar.l2_bg = "\033[48;2;60;60;60m";
        bar.l2_fg = "\033[38;2;120;120;120m";
    } else {
        bar.status = "RUNNING";
        bar.controls = "Ctrl+C cancel  Ctrl+\\ detach  Ctrl+V output ";
        bar.l2_bg = "\033[48;2;85;72;62m";
        bar.l2_fg = "\033[38;2;150;150;150m";
    }

    TerminalUI::draw_status_bar(bar);
}

// ── relay_loop ──────────────────────────────────────────────────

Result<int> JobView::relay_loop(ShellRelay& relay, RelayState& state) {
    LIBSSH2_CHANNEL* ch = relay.channel();
    auto io_mtx = relay.io_mutex();
    int sock = relay.socket();

    char buf[16384];
    std::string input_buffer;
    int cursor_pos = 0;
    std::string pending_suppress;
    bool job_started = false;

    bool user_detached = false;
    bool write_error = false;

    auto process_job_start = [&](std::string& display) {
        if (job_started) return;
        auto marker = display.find(JOB_START_MARKER);
        if (marker == std::string::npos) return;
        job_started = true;
        if (state.skip_remote_replay)
            write(STDOUT_FILENO, "\033[H\033[J", 6);
        size_t start = marker;
        size_t end = marker + JOB_START_MARKER_LEN;
        if (start > 0 && display[start - 1] == '\n') start--;
        if (end < display.size() && display[end] == '\n') end++;
        display.erase(start, end - start);
    };

    while (!write_error && !user_detached && !relay.done()) {
        bool stdin_ready = false;
        bool sock_ready = false;
#ifdef _WIN32
        stdin_ready = platform::poll_stdin(0);
        sock_ready = platform::poll_socket(sock, POLLIN, stdin_ready ? 0 : 100) & POLLIN;
#else
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {sock, POLLIN, 0};
        poll(fds, 2, 100);
        stdin_ready = fds[0].revents & POLLIN;
        sock_ready = fds[1].revents & POLLIN;
#endif

        // Handle window resize
        if (g_need_resize) {
            g_need_resize = 0;
            int new_cols = platform::term_width();
            int new_rows = platform::term_height();
            state.cols = new_cols;
            int new_pty = std::max(1, new_rows - TerminalUI::HEADER_ROWS);
            state.pty_rows = new_pty;
            std::string sr = fmt::format("\033[1;{}r", new_pty);
            write(STDOUT_FILENO, sr.data(), sr.size());
            draw_header();
            {
                std::lock_guard<std::mutex> lock(*io_mtx);
                libssh2_channel_request_pty_size(ch, new_cols, new_pty);
            }
        }

        // ── stdin → remote ──────────────────────────────────────

        if (stdin_ready) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                char c = buf[i];

                if (c == 0x1c) { user_detached = true; break; }     // Ctrl+\ → detach
                if (c == 0x03) return Result<int>::Ok(kCanceled);    // Ctrl+C → cancel
                if (c == 0x16) return Result<int>::Ok(kViewOutput);  // Ctrl+V → view output

                // Arrow keys
                if (c == 0x1b && i + 2 < n && buf[i+1] == '[') {
                    char seq = buf[i+2];
                    i += 2;
                    if (seq == 'D' && cursor_pos > 0) {
                        cursor_pos--;
                        write(STDOUT_FILENO, "\033[D", 3);
                    } else if (seq == 'C' && cursor_pos < (int)input_buffer.size()) {
                        cursor_pos++;
                        write(STDOUT_FILENO, "\033[C", 3);
                    }
                    continue;
                }

                // Backspace
                if (c == 127 || c == 8) {
                    if (cursor_pos > 0) {
                        input_buffer.erase(cursor_pos - 1, 1);
                        cursor_pos--;
                        std::string tail = input_buffer.substr(cursor_pos);
                        write(STDOUT_FILENO, "\b\033[K", 4);
                        if (!tail.empty()) {
                            write(STDOUT_FILENO, tail.c_str(), tail.size());
                            for (size_t j = 0; j < tail.size(); j++)
                                write(STDOUT_FILENO, "\b", 1);
                        }
                    }
                    continue;
                }

                // Enter — send buffered line
                if (c == '\r' || c == '\n') {
                    write(STDOUT_FILENO, "\r\n", 2);
                    pending_suppress = input_buffer + "\r\n";
                    input_buffer += '\n';

                    const char* send_buf = input_buffer.c_str();
                    int send_len = static_cast<int>(input_buffer.size());
                    int sent = 0;
                    while (sent < send_len) {
                        int w;
                        {
                            std::lock_guard<std::mutex> lock(*io_mtx);
                            w = libssh2_channel_write(ch, send_buf + sent, send_len - sent);
                        }
                        if (w == LIBSSH2_ERROR_EAGAIN) continue;
                        if (w < 0) { write_error = true; break; }
                        sent += w;
                    }
                    input_buffer.clear();
                    cursor_pos = 0;
                    continue;
                }

                // Regular character
                if (c >= 32 || c == '\t') {
                    input_buffer.insert(cursor_pos, 1, c);
                    cursor_pos++;
                    std::string tail = input_buffer.substr(cursor_pos - 1);
                    write(STDOUT_FILENO, tail.c_str(), tail.size());
                    for (size_t j = 1; j < tail.size(); j++)
                        write(STDOUT_FILENO, "\b", 1);
                }
            }
        }

        // ── remote → display ────────────────────────────────────

        if (sock_ready) {
            bool got_output = false;
            for (;;) {
                int n;
                {
                    std::lock_guard<std::mutex> lock(*io_mtx);
                    n = libssh2_channel_read(ch, buf, sizeof(buf));
                }
                if (n <= 0) break;

                // ShellRelay handles echo skip + done marker
                std::string text = relay.feed(buf, n);
                if (text.empty()) continue;

                // Strip ANSI for display
                std::string plain = TerminalOutput::strip_ansi(text.data(), static_cast<int>(text.size()));
                TerminalOutput::suppress_noise(plain);

                // Echo suppression for user-typed input
                if (!pending_suppress.empty() && !plain.empty()) {
                    if (plain.find(pending_suppress) == 0) {
                        plain = plain.substr(pending_suppress.size());
                        pending_suppress.clear();
                    } else if (pending_suppress.find(plain) == 0) {
                        pending_suppress = pending_suppress.substr(plain.size());
                        plain.clear();
                    } else {
                        pending_suppress.clear();
                    }
                }

                if (plain.empty()) continue;

                // Job start marker detection
                process_job_start(plain);

                if (!plain.empty()) {
                    write(STDOUT_FILENO, plain.data(), plain.size());
                    got_output = true;
                }

                if (relay.done()) break;
            }

            if (got_output) {
                got_first_output_ = true;
                std::string sr = fmt::format("\0337\033[1;{}r\0338", state.pty_rows);
                write(STDOUT_FILENO, sr.data(), sr.size());
                draw_header();
            }
        }

        {
            bool eof;
            {
                std::lock_guard<std::mutex> lock(*io_mtx);
                eof = libssh2_channel_eof(ch);
            }
            if (eof) break;
        }
    }

    if (user_detached)
        return Result<int>::Ok(kDetached);

    return Result<int>::Ok(relay.done() ? 0 : -1);
}

// ── attach ──────────────────────────────────────────────────────

Result<int> JobView::attach(bool skip_remote_replay) {
    if (compute_node_.empty())
        return Result<int>::Err("Job was canceled during initialization (no compute node assigned)");

    int cols = platform::term_width();
    int rows = platform::term_height();
    int pty_rows = std::max(1, rows - TerminalUI::HEADER_ROWS);

    std::cout.flush();

    // Set scroll region and draw initial header
    std::string sr = fmt::format("\0337\033[1;{}r\0338", pty_rows);
    write(STDOUT_FILENO, sr.data(), sr.size());
    draw_header();
    write(STDOUT_FILENO, "\033[H", 3);

    // Build SSH command to hop through DTN to compute node.
    // Single quotes so $TRIES/$HOME expand on the compute node, not the DTN.
    std::string log_path = dtach_socket_;
    auto dot = log_path.rfind('.');
    if (dot != std::string::npos)
        log_path = log_path.substr(0, dot) + ".log";

    std::string cmd = fmt::format(
        "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
        "'TRIES=0; while [ ! -e {} ] && [ $TRIES -lt 100 ]; do sleep 0.1; TRIES=$((TRIES+1)); done; "
        "cat {} 2>/dev/null; "
        "if [ -e {} ]; then $HOME/tccp/bin/dtach -a {} -r none; fi'",
        compute_node_, log_path, log_path, dtach_socket_, dtach_socket_);

    // ShellRelay handles: lock session, drain stale data, send command + done marker
    ShellRelay relay(factory_);
    if (!relay.start(cmd)) {
        write(STDOUT_FILENO, "\033[r", 3);
        return Result<int>::Err("No primary channel available");
    }

    // Setup terminal raw mode + resize handler
    platform::on_terminal_resize([] { g_need_resize = 1; });

    AttachCleanup cleanup;
    cleanup.raw_guard = new platform::RawModeGuard(platform::RawModeGuard::kFullRaw);
    cleanup.has_resize = true;

    RelayState state{cols, pty_rows, skip_remote_replay};
    auto result = relay_loop(relay, state);

    // ShellRelay::stop() handles: Ctrl+C if still running, drain, unlock
    relay.stop();
    return result;
}
