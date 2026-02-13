#include "job_view.hpp"
#include "terminal_output.hpp"
#include "terminal_ui.hpp"
#include <iostream>
#include <vector>
#include <libssh2.h>
#include <fmt/format.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <cstdio>

// JobView: Interactive terminal UI for viewing remote job execution
//
// Architecture:
//   - SSH channel on DTN → compute node → dtach session
//   - Bidirectional relay: stdin → remote, remote → stdout
//   - Line buffering + local editing on stdin
//   - Output filtering + scrollback buffer for viewport control
//   - Header shows status (Loading/RUNNING/TERMINATED)
//   - Scroll region protects header from remote output

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
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) w = ws.ws_col;
        if (ws.ws_row > 0) h = ws.ws_row;
    }

    // Top line: tccp | job_name | SLURM:id | node
    std::string l1_content = fmt::format(
        " \033[38;2;62;120;178;1mtccp\033[22;38;2;178;178;178m | {} | SLURM:{} | {}",
        job_name_, slurm_id_, compute_node_);
    std::string l1_plain = fmt::format(
        " tccp | {} | SLURM:{} | {}", job_name_, slurm_id_, compute_node_);
    int l1_pad = std::max(0, w - static_cast<int>(l1_plain.size()));

    // Bottom line: status + controls hint
    std::string l2_left, l2_right, l2_bg, l2_fg;

    if (terminated && canceled) {
        l2_left = " CANCELED";
        l2_right = "↑↓ scroll  Ctrl+T top  Ctrl+\\ exit ";
        l2_bg = "\033[48;2;60;60;60m";   // Grey
        l2_fg = "\033[38;2;120;120;120m";
    } else if (terminated) {
        l2_left = exit_code == 0 ? " TERMINATED" : fmt::format(" TERMINATED (exit {})", exit_code);
        l2_right = "↑↓ scroll  Ctrl+T top  Ctrl+\\ exit ";
        l2_bg = "\033[48;2;60;60;60m";   // Grey
        l2_fg = "\033[38;2;120;120;120m";
    } else if (!got_first_output_) {
        l2_left = " Loading...";
        l2_right = "";
        l2_bg = "\033[48;2;60;60;60m";   // Grey
        l2_fg = "\033[38;2;120;120;120m";
    } else {
        l2_left = " RUNNING";
        l2_right = "↑↓ scroll  Ctrl+T top  Ctrl+C cancel  Ctrl+\\ detach ";
        l2_bg = "\033[48;2;85;72;62m";   // Brown
        l2_fg = "\033[38;2;150;150;150m";
    }

    int l2_pad = std::max(0, w - static_cast<int>(l2_left.size()) - static_cast<int>(l2_right.size()));

    // Render header (save/restore cursor to avoid displacement)
    std::string out;
    out.reserve(256);
    out += "\0337";  // Save cursor

    out += fmt::format("\033[{};1H", h - 1);
    out += "\033[48;2;62;62;62m\033[38;2;178;178;178m";
    out += l1_content;
    out.append(l1_pad, ' ');
    out += "\033[0m";

    out += fmt::format("\033[{};1H", h);
    out += l2_bg + l2_fg;
    out += l2_left;
    out.append(l2_pad, ' ');
    out += l2_right;
    // Clear to end of line with background color still applied
    out += "\033[K";
    out += "\033[0m";

    out += "\0338";  // Restore cursor
    write(STDOUT_FILENO, out.data(), out.size());
}

Result<int> JobView::attach(bool skip_remote_replay) {
    // Check if job was canceled during init (no compute node assigned)
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
    write(STDOUT_FILENO, "\033[H", 3);  // Move cursor to top-left

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

    // Wait for log file (with timeout), tail it, then attach to dtach if alive
    std::string cmd = fmt::format(
        "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
        "\"TRIES=0; while [ ! -e {} ] && [ \\$TRIES -lt 100 ]; do sleep 0.1; TRIES=\\$((TRIES+1)); done; "
        "tail -c 65536 {} 2>/dev/null | perl -pe 's/\\e\\[[\\d;]*[HJK]//g'; "
        "if [ -e {} ]; then \\$HOME/tccp/bin/dtach -a {} -r none; fi\"",
        compute_node_, log_path, log_path, dtach_socket_, dtach_socket_);

    // Execute command
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

    // ── Relay loop ──────────────────────────────────────────────

    char buf[16384];
    std::string input_buffer;
    int cursor_pos = 0;
    std::string pending_suppress;

    std::vector<std::string> output_lines;
    int scroll_offset = 0;
    bool in_scroll_mode = false;
    bool should_clear_on_start = skip_remote_replay;
    bool job_started = false;
    int consecutive_newlines = 0;

    bool remote_eof = false;
    bool user_detached = false;
    bool write_error = false;

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
                int new_pty = std::max(1, static_cast<int>(rws.ws_row) - TerminalUI::HEADER_ROWS);
                std::string sr2 = fmt::format("\033[1;{}r", new_pty);
                write(STDOUT_FILENO, sr2.data(), sr2.size());
                draw_header();
                libssh2_channel_request_pty_size(channel, rws.ws_col, new_pty);
            }
        }

        // stdin → remote (line buffering + local editing)
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
                    return Result<int>::Ok(-2);  // Signal: cancel requested
                }

                // Ctrl+T → scroll to top (show full screen)
                if (c == 0x14) {
                    in_scroll_mode = true;
                    scroll_offset = TerminalUI::compute_top_scroll(output_lines.size());
                    TerminalUI::redraw_viewport(output_lines, scroll_offset);
                    draw_header();
                    continue;
                }

                // Escape sequences (arrow keys)
                if (c == 0x1b && i + 2 < n && buf[i+1] == '[') {
                    char seq = buf[i+2];
                    i += 2;

                    if (seq == 'A') {  // Up arrow - scroll back
                        in_scroll_mode = true;
                        if (scroll_offset < TerminalUI::compute_max_scroll(output_lines.size())) {
                            scroll_offset++;
                            TerminalUI::redraw_viewport(output_lines, scroll_offset);
                            draw_header();
                        }
                        continue;
                    } else if (seq == 'B') {  // Down arrow - scroll forward
                        if (scroll_offset > 0) {
                            scroll_offset--;
                            TerminalUI::redraw_viewport(output_lines, scroll_offset);
                            draw_header();
                            if (scroll_offset == 0) {
                                in_scroll_mode = false;
                                // Reprint input buffer at bottom
                                if (!input_buffer.empty()) {
                                    write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                    int moves_back = input_buffer.size() - cursor_pos;
                                    for (int j = 0; j < moves_back; j++)
                                        write(STDOUT_FILENO, "\033[D", 3);
                                }
                            }
                        }
                        continue;
                    } else if (seq == 'D') {  // Left arrow
                        if (in_scroll_mode) {
                            scroll_offset = 0;
                            in_scroll_mode = false;
                            TerminalUI::redraw_viewport(output_lines, 0);
                            draw_header();
                            if (!input_buffer.empty()) {
                                write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                int moves_back = input_buffer.size() - cursor_pos;
                                for (int j = 0; j < moves_back; j++)
                                    write(STDOUT_FILENO, "\033[D", 3);
                            }
                        }
                        if (cursor_pos > 0) {
                            cursor_pos--;
                            write(STDOUT_FILENO, "\033[D", 3);
                        }
                    } else if (seq == 'C') {  // Right arrow
                        if (in_scroll_mode) {
                            scroll_offset = 0;
                            in_scroll_mode = false;
                            TerminalUI::redraw_viewport(output_lines, 0);
                            draw_header();
                            if (!input_buffer.empty()) {
                                write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                int moves_back = input_buffer.size() - cursor_pos;
                                for (int j = 0; j < moves_back; j++)
                                    write(STDOUT_FILENO, "\033[D", 3);
                            }
                        }
                        if (cursor_pos < (int)input_buffer.size()) {
                            cursor_pos++;
                            write(STDOUT_FILENO, "\033[C", 3);
                        }
                    }
                    continue;
                }

                // Exit scroll mode on any non-scroll input
                if (in_scroll_mode) {
                    scroll_offset = 0;
                    in_scroll_mode = false;
                    TerminalUI::redraw_viewport(output_lines, 0);
                    draw_header();
                    if (!input_buffer.empty()) {
                        write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                        int moves_back = input_buffer.size() - cursor_pos;
                        for (int j = 0; j < moves_back; j++)
                            write(STDOUT_FILENO, "\033[D", 3);
                    }
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

                // Enter - send buffered line
                if (c == '\r' || c == '\n') {
                    write(STDOUT_FILENO, "\r\n", 2);
                    // Add user input to scrollback (must use \r\n in raw mode)
                    output_lines.push_back(input_buffer + "\r\n");
                    pending_suppress = input_buffer + "\r\n";
                    input_buffer += '\n';

                    // Send to remote
                    const char* send_buf = input_buffer.c_str();
                    int send_len = input_buffer.size();
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

                // Regular character - insert at cursor
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

        // remote → stdout + capture
        if (fds[1].revents & POLLIN) {
            bool got_output = false;
            for (;;) {
                int n = libssh2_channel_read(channel, buf, sizeof(buf));
                if (n <= 0) break;

                TerminalOutput::process_channel_output(buf, n, output_lines, job_started,
                                       should_clear_on_start, consecutive_newlines,
                                       capture, !in_scroll_mode, &pending_suppress);
                got_output = true;
            }

            if (got_output && !in_scroll_mode) {
                got_first_output_ = true;
                // Re-assert scroll region (remote output may reset it)
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
        // Drain remaining buffered output
        for (;;) {
            int n = libssh2_channel_read(channel, buf, sizeof(buf));
            if (n <= 0) break;
            TerminalOutput::process_channel_output(buf, n, output_lines, job_started,
                                   should_clear_on_start, consecutive_newlines,
                                   capture, false, nullptr);
        }

        TerminalUI::wait_for_exit_with_scroll(output_lines, exit_status, [this, exit_status]() {
            draw_header(true, exit_status, canceled_);
        });
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
