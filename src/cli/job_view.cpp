#include "job_view.hpp"
#include "terminal_output.hpp"
#include "terminal_ui.hpp"
#include <iostream>
#include <libssh2.h>
#include <fmt/format.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <cstdio>

// JobView: Simple interactive relay for remote job execution.
//
// Data flow:
//   Remote SSH bytes → strip_ansi() → plain text → terminal + capture file
//   Local input      → line-buffered editing → send on Enter
//   Status bar       → separate UI chrome at bottom

// ── SIGWINCH handler ────────────────────────────────────────────

static volatile sig_atomic_t g_need_resize = 0;
static void handle_sigwinch(int) { g_need_resize = 1; }

// ── JobView implementation ──────────────────────────────────────

JobView::JobView(SessionManager& session, const std::string& compute_node,
                 const std::string& username, const std::string& dtach_socket,
                 const std::string& job_name, const std::string& slurm_id,
                 const std::string& output_file, const std::string& job_id,
                 bool canceled)
    : session_(session), compute_node_(compute_node),
      username_(username), dtach_socket_(dtach_socket),
      job_name_(job_name), slurm_id_(slurm_id),
      output_file_(output_file), job_id_(job_id), canceled_(canceled) {}

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
        bar.controls = "Ctrl+C cancel  Ctrl+\\ detach ";
        bar.l2_bg = "\033[48;2;85;72;62m";
        bar.l2_fg = "\033[38;2;150;150;150m";
    }

    TerminalUI::draw_status_bar(bar);
}

Result<int> JobView::attach(bool skip_remote_replay) {
    if (compute_node_.empty()) {
        return Result<int>::Err("Job was canceled during initialization (no compute node assigned)");
    }

    LIBSSH2_SESSION* ssh = session_.get_raw_session();
    int sock = session_.get_socket();

    FILE* capture = nullptr;
    if (!output_file_.empty())
        capture = fopen(output_file_.c_str(), "a");

    // Get terminal dimensions
    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    int pty_rows = std::max(1, rows - TerminalUI::HEADER_ROWS);

    std::cout.flush();

    // Set scroll region and draw initial header
    std::string sr = fmt::format("\0337\033[1;{}r\0338", pty_rows);
    write(STDOUT_FILENO, sr.data(), sr.size());
    draw_header();
    write(STDOUT_FILENO, "\033[H", 3);

    // ── Open SSH channel ────────────────────────────────────────

    LIBSSH2_CHANNEL* channel = nullptr;
    while (!(channel = libssh2_channel_open_session(ssh))) {
        if (libssh2_session_last_errno(ssh) != LIBSSH2_ERROR_EAGAIN) {
            if (capture) fclose(capture);
            write(STDOUT_FILENO, "\033[r", 3);
            return Result<int>::Err("Failed to open channel on DTN");
        }
        usleep(100000);
    }

    int rc;
    while ((rc = libssh2_channel_request_pty_ex(
                channel, "xterm-256color", 14,
                nullptr, 0, cols, pty_rows, 0, 0)) == LIBSSH2_ERROR_EAGAIN)
        usleep(100000);

    if (rc != 0) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        if (capture) fclose(capture);
        write(STDOUT_FILENO, "\033[r", 3);
        return Result<int>::Err("Failed to request PTY on DTN channel");
    }

    // ── Build SSH command ───────────────────────────────────────

    std::string log_path = dtach_socket_;
    auto dot = log_path.rfind('.');
    if (dot != std::string::npos)
        log_path = log_path.substr(0, dot) + ".log";

    std::string cmd = fmt::format(
        "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
        "\"TRIES=0; while [ ! -e {} ] && [ \\$TRIES -lt 100 ]; do sleep 0.1; TRIES=\\$((TRIES+1)); done; "
        "cat {} 2>/dev/null | perl -pe 's/\\e\\[[\\d;]*[HJK]//g'; "
        "if [ -e {} ]; then \\$HOME/tccp/bin/dtach -a {} -r none; fi\"",
        compute_node_, log_path, log_path, dtach_socket_, dtach_socket_);

    while ((rc = libssh2_channel_exec(channel, cmd.c_str())) == LIBSSH2_ERROR_EAGAIN)
        usleep(100000);

    if (rc != 0) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        if (capture) fclose(capture);
        write(STDOUT_FILENO, "\033[r", 3);
        return Result<int>::Err("Failed to exec SSH command");
    }

    // ── Setup terminal raw mode ─────────────────────────────────

    struct sigaction sa, old_sa;
    sa.sa_handler = handle_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, &old_sa);

    struct termios old_term, raw_term;
    tcgetattr(STDIN_FILENO, &old_term);
    raw_term = old_term;
    cfmakeraw(&raw_term);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);

    // ── Relay state ─────────────────────────────────────────────

    char buf[16384];
    std::string input_buffer;
    int cursor_pos = 0;
    std::string pending_suppress;
    bool job_started = false;

    bool remote_eof = false;
    bool user_detached = false;
    bool write_error = false;

    // ── Relay loop ──────────────────────────────────────────────

    while (!write_error && !user_detached) {
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {sock, POLLIN, 0};
        poll(fds, 2, 100);

        // Handle window resize
        if (g_need_resize) {
            g_need_resize = 0;
            struct winsize rws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &rws) == 0) {
                cols = rws.ws_col;
                int new_pty = std::max(1, static_cast<int>(rws.ws_row) - TerminalUI::HEADER_ROWS);
                pty_rows = new_pty;
                std::string sr2 = fmt::format("\033[1;{}r", new_pty);
                write(STDOUT_FILENO, sr2.data(), sr2.size());
                draw_header();
                libssh2_channel_request_pty_size(channel, rws.ws_col, new_pty);
            }
        }

        // ── stdin → remote (line buffering + local editing) ─────

        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                char c = buf[i];

                // Ctrl+\ → detach
                if (c == 0x1c) {
                    user_detached = true;
                    break;
                }

                // Ctrl+C → cancel job
                if (c == 0x03) {
                    libssh2_channel_close(channel);
                    libssh2_channel_free(channel);
                    if (capture) fclose(capture);
                    write(STDOUT_FILENO, "\033[r", 3);
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                    sigaction(SIGWINCH, &old_sa, nullptr);
                    return Result<int>::Ok(-2);
                }

                // Escape sequences (arrow keys for cursor editing)
                if (c == 0x1b && i + 2 < n && buf[i+1] == '[') {
                    char seq = buf[i+2];
                    i += 2;

                    if (seq == 'D') {  // Left arrow
                        if (cursor_pos > 0) {
                            cursor_pos--;
                            write(STDOUT_FILENO, "\033[D", 3);
                        }
                    } else if (seq == 'C') {  // Right arrow
                        if (cursor_pos < (int)input_buffer.size()) {
                            cursor_pos++;
                            write(STDOUT_FILENO, "\033[C", 3);
                        }
                    }
                    // Up/Down arrows: ignored (no scroll, no history)
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
                        int w = libssh2_channel_write(channel, send_buf + sent, send_len - sent);
                        if (w == LIBSSH2_ERROR_EAGAIN) continue;
                        if (w < 0) { write_error = true; break; }
                        sent += w;
                    }

                    input_buffer.clear();
                    cursor_pos = 0;
                    continue;
                }

                // Regular character — insert at cursor
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

        // ── remote → display + capture ──────────────────────────

        if (fds[1].revents & POLLIN) {
            bool got_output = false;
            for (;;) {
                int n = libssh2_channel_read(channel, buf, sizeof(buf));
                if (n <= 0) break;

                // Strip ANSI for clean text
                std::string plain = TerminalOutput::strip_ansi(buf, n);
                TerminalOutput::suppress_noise(plain);

                // Echo suppression
                if (!pending_suppress.empty() && !plain.empty()) {
                    if (plain.find(pending_suppress) == 0) {
                        plain = plain.substr(pending_suppress.size());
                        pending_suppress.clear();
                    } else if (pending_suppress.find(plain) == 0 &&
                               pending_suppress.substr(0, plain.size()) == plain) {
                        pending_suppress = pending_suppress.substr(plain.size());
                        plain.clear();
                    } else {
                        pending_suppress.clear();
                    }
                }

                // Detect job start marker
                if (!job_started) {
                    auto marker = plain.find("__TCCP_JOB_START__");
                    if (marker != std::string::npos) {
                        job_started = true;

                        if (skip_remote_replay)
                            write(STDOUT_FILENO, "\033[H\033[J", 6);

                        size_t start = marker;
                        size_t end = marker + 18;
                        if (start > 0 && plain[start - 1] == '\n') start--;
                        if (end < plain.size() && plain[end] == '\n') end++;
                        plain.erase(start, end - start);
                    }
                }

                if (!plain.empty()) {
                    // Write to terminal
                    write(STDOUT_FILENO, plain.data(), plain.size());

                    // Write to capture file
                    if (capture) {
                        fwrite(plain.data(), 1, plain.size(), capture);
                        fflush(capture);
                    }

                    got_output = true;
                }
            }

            if (got_output) {
                got_first_output_ = true;
                // Re-assert scroll region + redraw header
                std::string sr2 = fmt::format("\0337\033[1;{}r\0338", pty_rows);
                write(STDOUT_FILENO, sr2.data(), sr2.size());
                draw_header();
            }
        }

        if (libssh2_channel_eof(channel)) {
            remote_eof = true;
            break;
        }
    }

    // ── Post-relay ──────────────────────────────────────────────

    int exit_status = libssh2_channel_get_exit_status(channel);
    bool job_finished = remote_eof && !user_detached;

    if (job_finished) {
        // Drain remaining output
        for (;;) {
            int n = libssh2_channel_read(channel, buf, sizeof(buf));
            if (n <= 0) break;
            std::string plain = TerminalOutput::strip_ansi(buf, n);
            TerminalOutput::suppress_noise(plain);
            if (!plain.empty()) {
                write(STDOUT_FILENO, plain.data(), plain.size());
                if (capture) {
                    fwrite(plain.data(), 1, plain.size(), capture);
                    fflush(capture);
                }
            }
        }
    }

    // ── Cleanup ─────────────────────────────────────────────────

    if (capture) fclose(capture);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    sigaction(SIGWINCH, &old_sa, nullptr);

    libssh2_channel_send_eof(channel);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);

    write(STDOUT_FILENO, "\033[r", 3);

    if (user_detached)
        return Result<int>::Ok(-1);

    return Result<int>::Ok(exit_status);
}
