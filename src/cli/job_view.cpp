#include "job_view.hpp"
#include <iostream>
#include <vector>
#include <libssh2.h>
#include <fmt/format.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstring>

// ── Constants ───────────────────────────────────────────────

static constexpr int HEADER_ROWS = 2;

// ── SIGWINCH ────────────────────────────────────────────────

static volatile sig_atomic_t g_need_resize = 0;

static void handle_sigwinch(int) { g_need_resize = 1; }

// ── Output filters & processors ────────────────────────────

// Keep text + SGR colors (\033[...m), strip all other CSI sequences and \r.
static std::string filter_for_capture(const char* buf, int len) {
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ) {
        if (buf[i] == '\033' && i + 1 < len && buf[i+1] == '[') {
            int j = i + 2;
            while (j < len && (buf[j] == ';' || (buf[j] >= '0' && buf[j] <= '9') || buf[j] == '?'))
                j++;
            if (j < len) {
                if (buf[j] == 'm')
                    out.append(buf + i, j + 1 - i);
                i = j + 1;
            } else {
                i = j;
            }
        } else if (buf[i] == '\033') {
            i += 2;
        } else if (buf[i] == '\r') {
            i++;
        } else {
            out += buf[i++];
        }
    }
    return out;
}

// Erase all occurrences of a literal byte sequence.
static void strip_seq(std::string& s, const char* seq) {
    size_t len = strlen(seq);
    std::string::size_type pos;
    while ((pos = s.find(seq)) != std::string::npos)
        s.erase(pos, len);
}

// Remove the entire line containing `pat`.
static void suppress_line(std::string& s, const char* pat) {
    auto pos = s.find(pat);
    if (pos == std::string::npos) return;
    auto start = s.rfind('\n', pos);
    start = (start == std::string::npos) ? 0 : start;
    auto end = s.find('\n', pos);
    end = (end == std::string::npos) ? s.size() : end + 1;
    s.erase(start, end - start);
}

// Filter raw channel output for display:
// 1. Collapse runs of >2 consecutive blank lines
// 2. Strip destructive screen sequences (alt screen, clear, cursor home)
// 3. Remove noise lines (dtach, ssh, script)
static std::string filter_for_display(const char* buf, int n,
                                       int& consecutive_newlines) {
    std::string display;
    display.reserve(n);
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (++consecutive_newlines <= 2) display += buf[i];
        } else if (buf[i] == '\r') {
            display += buf[i];
        } else {
            consecutive_newlines = 0;
            display += buf[i];
        }
    }

    strip_seq(display, "\033[?1049h");
    strip_seq(display, "\033[?1049l");
    strip_seq(display, "\033[?47h");
    strip_seq(display, "\033[?47l");
    strip_seq(display, "\033[2J");
    strip_seq(display, "\033[H");

    suppress_line(display, "EOF - dtach");
    suppress_line(display, "[detached]");
    suppress_line(display, "Connection to");
    suppress_line(display, "Script started on");
    suppress_line(display, "Script done on");

    return display;
}

// Process channel output: filter, detect markers, accumulate in buffer, write to capture
static void process_channel_output(
    const char* buf, int n,
    std::vector<std::string>& output_lines,
    bool& job_started,
    bool should_clear_on_start,
    int& consecutive_newlines,
    FILE* capture,
    bool write_to_screen,
    std::string* pending_suppress = nullptr)
{
    std::string display = filter_for_display(buf, n, consecutive_newlines);

    // Suppress remote echo if requested
    if (pending_suppress && !pending_suppress->empty() && !display.empty()) {
        if (display.find(*pending_suppress) == 0) {
            display = display.substr(pending_suppress->size());
            pending_suppress->clear();
        } else if (pending_suppress->find(display) == 0) {
            if (pending_suppress->size() >= display.size() &&
                pending_suppress->substr(0, display.size()) == display) {
                *pending_suppress = pending_suppress->substr(display.size());
                display.clear();
            }
        } else {
            pending_suppress->clear();
        }
    }

    if (!display.empty()) {
        // Detect job start marker
        if (!job_started && display.find("__TCCP_JOB_START__") != std::string::npos) {
            job_started = true;

            if (should_clear_on_start && write_to_screen) {
                write(STDOUT_FILENO, "\033[H\033[J", 6);
                output_lines.clear();
            }

            // Strip marker from display
            size_t marker_pos = display.find("__TCCP_JOB_START__");
            if (marker_pos != std::string::npos) {
                size_t start = marker_pos;
                size_t end = marker_pos + 18;
                if (start > 0 && display[start - 1] == '\n') start--;
                if (end < display.size() && display[end] == '\n') end++;
                display.erase(start, end - start);
            }
        }

        // Add to scrollback buffer (split by lines)
        size_t pos = 0;
        size_t newline_pos;
        while ((newline_pos = display.find('\n', pos)) != std::string::npos) {
            std::string line = display.substr(pos, newline_pos - pos + 1);
            output_lines.push_back(line);
            pos = newline_pos + 1;
        }
        // Add remaining partial line
        if (pos < display.size()) {
            if (!output_lines.empty()) {
                output_lines.back() += display.substr(pos);
            } else {
                output_lines.push_back(display.substr(pos));
            }
        }

        // Write to screen if requested
        if (write_to_screen) {
            write(STDOUT_FILENO, display.data(), display.size());
        }

        // Write to capture file
        if (capture) {
            std::string filtered = filter_for_capture(display.data(), display.size());
            fwrite(filtered.data(), 1, filtered.size(), capture);
            fflush(capture);
        }
    }
}

// ── Scroll helpers ──────────────────────────────────────────

static int get_scroll_height() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return ws.ws_row - HEADER_ROWS;
    }
    return 24 - HEADER_ROWS;
}

// Compute max scroll offset: allow scrolling until just 1 line remains visible.
static int compute_max_scroll(int total_lines) {
    return std::max(0, total_lines - 1);
}

// Redraw the visible portion of output_lines based on scroll_offset
static void redraw_viewport(const std::vector<std::string>& output_lines, int scroll_offset) {
    int height = get_scroll_height();
    int total_lines = output_lines.size();

    // Clear scroll area and move to top
    write(STDOUT_FILENO, "\033[H\033[J", 6);  // Home + clear to end

    // Calculate which lines to show
    int end_line = total_lines - scroll_offset;
    int start_line = std::max(0, end_line - height);

    // Show scroll indicator at top if we're scrolled back and at the beginning
    bool showing_top_indicator = (scroll_offset > 0 && start_line == 0);
    if (showing_top_indicator) {
        std::string indicator = "\033[2m[TOP]\033[0m\r\n";
        write(STDOUT_FILENO, indicator.c_str(), indicator.size());
        // [TOP] takes up one line, reduce content range but always keep at least 1 line
        if (end_line - start_line > 1) end_line--;
    }

    // Draw visible lines (they already contain their newlines)
    for (int i = start_line; i < end_line && i < total_lines; i++) {
        write(STDOUT_FILENO, output_lines[i].c_str(), output_lines[i].size());
    }
}

// Show exit prompt and allow scrolling through output buffer. Returns on Enter.
static void wait_for_exit_with_scroll(
    std::vector<std::string>& output_lines,
    int exit_status,
    std::function<void()> draw_header_fn)
{
    int scroll_offset = 0;

    // Draw initial view at bottom
    redraw_viewport(output_lines, scroll_offset);

    // Position exit prompt within the scroll region (above header)
    struct winsize ews;
    int erows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ews) == 0 && ews.ws_row > 0)
        erows = ews.ws_row;
    int prompt_row = std::max(1, erows - HEADER_ROWS);

    auto draw_exit_prompt = [&]() {
        std::string exit_msg;
        if (exit_status == 0)
            exit_msg = fmt::format("\033[{};1H\033[2m[exit 0]\033[0m  \033[2mPress Enter to leave, \xe2\x86\x91\xe2\x86\x93 to scroll...\033[0m", prompt_row);
        else
            exit_msg = fmt::format("\033[{};1H\033[91m[exit {}]\033[0m  \033[2mPress Enter to leave, \xe2\x86\x91\xe2\x86\x93 to scroll...\033[0m", prompt_row, exit_status);
        write(STDOUT_FILENO, exit_msg.data(), exit_msg.size());
        draw_header_fn();
    };

    draw_exit_prompt();

    for (;;) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;

        if (c == 0x1b) {
            char seq[2];
            if (read(STDIN_FILENO, seq, 2) == 2 && seq[0] == '[') {
                if (seq[1] == 'A') {  // Up arrow
                    int max_scroll = compute_max_scroll(output_lines.size());
                    if (scroll_offset < max_scroll) {
                        scroll_offset++;
                        redraw_viewport(output_lines, scroll_offset);
                        draw_exit_prompt();
                    }
                } else if (seq[1] == 'B') {  // Down arrow
                    if (scroll_offset > 0) {
                        scroll_offset--;
                        redraw_viewport(output_lines, scroll_offset);
                        draw_exit_prompt();
                    }
                }
            }
            continue;
        }

        if (c == '\r' || c == '\n') break;
    }
}

// ── JobView ─────────────────────────────────────────────────

JobView::JobView(SessionManager& session, const std::string& compute_node,
                 const std::string& username, const std::string& dtach_socket,
                 const std::string& job_name, const std::string& slurm_id,
                 const std::string& output_file)
    : session_(session), compute_node_(compute_node),
      username_(username), dtach_socket_(dtach_socket),
      job_name_(job_name), slurm_id_(slurm_id),
      output_file_(output_file) {}

void JobView::draw_header() {
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) w = ws.ws_col;
        if (ws.ws_row > 0) h = ws.ws_row;
    }

    std::string l1_content = fmt::format(
        " \033[38;2;62;120;178;1mtccp\033[22;38;2;178;178;178m | {} | SLURM:{} | {}",
        job_name_, slurm_id_, compute_node_);
    std::string l1_plain = fmt::format(
        " tccp | {} | SLURM:{} | {}", job_name_, slurm_id_, compute_node_);
    int l1_pad = std::max(0, w - static_cast<int>(l1_plain.size()));

    std::string l2_left = " RUNNING";
    std::string l2_right = "Ctrl+\\ to detach ";
    int l2_pad = std::max(0, w - static_cast<int>(l2_left.size())
                                - static_cast<int>(l2_right.size()));

    std::string out;
    out.reserve(256);
    out += "\0337";  // save cursor

    out += fmt::format("\033[{};1H", h - 1);
    out += "\033[48;2;62;62;62m\033[38;2;178;178;178m";
    out += l1_content;
    out.append(l1_pad, ' ');
    out += "\033[0m";

    out += fmt::format("\033[{};1H", h);
    out += "\033[48;2;85;72;62m\033[38;2;150;150;150m";
    out += l2_left;
    out.append(l2_pad, ' ');
    out += "\033[38;2;120;120;120m" + l2_right;
    out += "\033[0m";

    out += "\0338";  // restore cursor
    write(STDOUT_FILENO, out.data(), out.size());
}

// ── attach() ────────────────────────────────────────────────

Result<int> JobView::attach(bool skip_remote_replay) {
    LIBSSH2_SESSION* ssh = session_.get_raw_session();
    int sock = session_.get_socket();

    FILE* capture = nullptr;
    if (!output_file_.empty())
        capture = fopen(output_file_.c_str(), "a");

    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    int pty_rows = std::max(1, rows - HEADER_ROWS);

    std::cout.flush();

    // Set scroll region, preserve cursor position from prior output
    std::string sr = fmt::format("\0337\033[1;{}r\0338", pty_rows);
    write(STDOUT_FILENO, sr.data(), sr.size());
    draw_header();
    write(STDOUT_FILENO, "\033[H", 3);  // Move cursor to top-left of scroll region

    // ── Open channel ──────────────────────────────────────

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

    // ── Build SSH command ─────────────────────────────────

    std::string log_path = dtach_socket_;
    auto dot = log_path.rfind('.');
    if (dot != std::string::npos)
        log_path = log_path.substr(0, dot) + ".log";

    // Always tail the log, then attach to dtach if session still exists
    std::string cmd = fmt::format(
        "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
        "\"while [ ! -e {} ]; do sleep 0.1; done 2>/dev/null || true; "
        "tail -c 65536 {} 2>/dev/null | perl -pe 's/\\e\\[[\\d;]*[HJK]//g'; "
        "if [ -e {} ]; then \\$HOME/tccp/bin/dtach -a {} -r none; fi\"",
        compute_node_, log_path, log_path, dtach_socket_, dtach_socket_);

    while ((rc = libssh2_channel_exec(channel, cmd.c_str())) == LIBSSH2_ERROR_EAGAIN)
        usleep(100000);

    if (rc != 0) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
        if (capture) fclose(capture);
        write(STDOUT_FILENO, "\033[r", 3);
        return Result<int>::Err("Failed to execute SSH to compute node");
    }

    // ── Terminal raw mode + SIGWINCH ──────────────────────

    struct sigaction sa, old_sa;
    sa.sa_handler = handle_sigwinch;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, &old_sa);

    struct termios old_term, raw_term;
    tcgetattr(STDIN_FILENO, &old_term);
    raw_term = old_term;
    cfmakeraw(&raw_term);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);

    // ── Relay loop ────────────────────────────────────────

    char buf[16384];
    std::string input_buffer;      // Buffer for line editing
    int cursor_pos = 0;            // Cursor position in buffer
    std::string pending_suppress;  // Suppress this from remote echo

    // Scrollback buffer - store all output lines
    std::vector<std::string> output_lines;
    bool in_scroll_mode = false;
    int scroll_offset = 0;         // How many lines scrolled back (0 = at bottom)
    // Only clear on job start marker if this is the FIRST attach (not a reattach)
    bool should_clear_on_start = !skip_remote_replay;
    bool job_started = false;      // True once we detect job actually running

    bool remote_eof = false;
    bool user_detached = false;
    bool write_error = false;
    int consecutive_newlines = 0;

    while (!write_error && !user_detached) {
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {sock, POLLIN, 0};
        poll(fds, 2, 100);

        if (g_need_resize) {
            g_need_resize = 0;
            struct winsize rws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &rws) == 0) {
                int new_pty = std::max(1, static_cast<int>(rws.ws_row) - HEADER_ROWS);
                std::string sr2 = fmt::format("\033[1;{}r", new_pty);
                write(STDOUT_FILENO, sr2.data(), sr2.size());
                draw_header();
                libssh2_channel_request_pty_size(channel, rws.ws_col, new_pty);
            }
        }

        // stdin → remote (with line buffering and local editing)
        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                unsigned char c = buf[i];

                // Check for Ctrl+\ detach
                if (c == 0x1c) {
                    user_detached = true;
                    break;
                }

                // Handle escape sequences (arrow keys, etc.)
                if (c == 0x1b && i + 2 < n && buf[i+1] == '[') {
                    char seq = buf[i+2];
                    i += 2;

                    if (seq == 'A') {  // Up arrow - scroll back
                        in_scroll_mode = true;
                        if (scroll_offset < compute_max_scroll(output_lines.size())) {
                            scroll_offset++;
                            redraw_viewport(output_lines, scroll_offset);
                            draw_header();
                        }
                        continue;
                    } else if (seq == 'B') {  // Down arrow - scroll forward to view newer output
                        if (scroll_offset > 0) {
                            scroll_offset--;
                            redraw_viewport(output_lines, scroll_offset);
                            draw_header();
                            if (scroll_offset == 0) {
                                in_scroll_mode = false;
                                // Reprint input buffer at bottom
                                if (!input_buffer.empty()) {
                                    write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                    // Move cursor back to correct position
                                    int moves_back = input_buffer.size() - cursor_pos;
                                    for (int j = 0; j < moves_back; j++) {
                                        write(STDOUT_FILENO, "\033[D", 3);
                                    }
                                }
                            }
                        }
                        continue;
                    } else if (seq == 'D') {  // Left arrow (line editing)
                        if (in_scroll_mode) {
                            // Exit scroll mode - redraw at bottom
                            scroll_offset = 0;
                            in_scroll_mode = false;
                            redraw_viewport(output_lines, 0);
                            draw_header();
                            // Reprint input buffer at bottom
                            if (!input_buffer.empty()) {
                                write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                // Move cursor back to correct position
                                int moves_back = input_buffer.size() - cursor_pos;
                                for (int j = 0; j < moves_back; j++) {
                                    write(STDOUT_FILENO, "\033[D", 3);
                                }
                            }
                        }
                        if (cursor_pos > 0) {
                            cursor_pos--;
                            write(STDOUT_FILENO, "\033[D", 3);
                        }
                    } else if (seq == 'C') {  // Right arrow (line editing)
                        if (in_scroll_mode) {
                            // Exit scroll mode - redraw at bottom
                            scroll_offset = 0;
                            in_scroll_mode = false;
                            redraw_viewport(output_lines, 0);
                            draw_header();
                            // Reprint input buffer at bottom
                            if (!input_buffer.empty()) {
                                write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                                // Move cursor back to correct position
                                int moves_back = input_buffer.size() - cursor_pos;
                                for (int j = 0; j < moves_back; j++) {
                                    write(STDOUT_FILENO, "\033[D", 3);
                                }
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
                    redraw_viewport(output_lines, 0);
                    draw_header();
                    // Reprint input buffer at bottom
                    if (!input_buffer.empty()) {
                        write(STDOUT_FILENO, input_buffer.c_str(), input_buffer.size());
                        // Move cursor back to correct position
                        int moves_back = input_buffer.size() - cursor_pos;
                        for (int j = 0; j < moves_back; j++) {
                            write(STDOUT_FILENO, "\033[D", 3);
                        }
                    }
                }

                // Handle backspace/delete
                if (c == 127 || c == 8) {
                    if (cursor_pos > 0) {
                        input_buffer.erase(cursor_pos - 1, 1);
                        cursor_pos--;
                        // Visual feedback: backspace, clear to end, rewrite tail, move cursor back
                        write(STDOUT_FILENO, "\b", 1);
                        std::string tail = input_buffer.substr(cursor_pos);
                        write(STDOUT_FILENO, tail.c_str(), tail.size());
                        write(STDOUT_FILENO, " \b", 2);
                        for (size_t j = 0; j < tail.size(); j++)
                            write(STDOUT_FILENO, "\b", 1);
                    }
                    continue;
                }

                // Handle Enter - send buffered line
                if (c == '\r' || c == '\n') {
                    write(STDOUT_FILENO, "\r\n", 2);

                    // Add user input to scrollback buffer so it's visible when scrolling.
                    // Must use \r\n (not bare \n) because terminal is in raw mode.
                    output_lines.push_back(input_buffer + "\r\n");

                    // Mark this input for suppression (remote will echo it back)
                    pending_suppress = input_buffer + "\r\n";

                    input_buffer += '\n';

                    // Send entire line to remote
                    int offset = 0;
                    int len = input_buffer.size();
                    while (offset < len) {
                        int w = libssh2_channel_write(channel,
                                                       input_buffer.c_str() + offset,
                                                       len - offset);
                        if (w == LIBSSH2_ERROR_EAGAIN) { usleep(1000); continue; }
                        if (w < 0) { write_error = true; break; }
                        offset += w;
                    }

                    // Clear buffer
                    input_buffer.clear();
                    cursor_pos = 0;
                    continue;
                }

                // Regular character - insert at cursor with local echo
                if (c >= 32 || c == '\t') {
                    input_buffer.insert(cursor_pos, 1, c);
                    cursor_pos++;

                    // Visual feedback: echo and redraw tail
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

                process_channel_output(buf, n, output_lines, job_started,
                                       should_clear_on_start, consecutive_newlines,
                                       capture, !in_scroll_mode, &pending_suppress);
                got_output = true;
            }

            if (got_output && !in_scroll_mode) {
                // Re-assert scroll region (remote output may reset it)
                std::string sr = fmt::format("\0337\033[1;{}r\0338", pty_rows);
                write(STDOUT_FILENO, sr.data(), sr.size());
                draw_header();
            }
        }

        if (libssh2_channel_eof(channel)) {
            remote_eof = true;
            break;
        }
    }

    // ── Post-relay ────────────────────────────────────────

    int exit_status = libssh2_channel_get_exit_status(channel);
    bool job_finished = remote_eof && !user_detached;

    if (job_finished) {
        // Drain and process any remaining buffered output
        for (;;) {
            int n = libssh2_channel_read(channel, buf, sizeof(buf));
            if (n <= 0) break;

            process_channel_output(buf, n, output_lines, job_started,
                                   should_clear_on_start, consecutive_newlines,
                                   capture, false, nullptr);
        }

        wait_for_exit_with_scroll(output_lines, exit_status, [this]() { draw_header(); });
    }

    // ── Cleanup ───────────────────────────────────────────

    if (capture) fclose(capture);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    sigaction(SIGWINCH, &old_sa, nullptr);

    while (libssh2_channel_close(channel) == LIBSSH2_ERROR_EAGAIN)
        usleep(10000);
    while (libssh2_channel_wait_closed(channel) == LIBSSH2_ERROR_EAGAIN)
        usleep(10000);
    libssh2_channel_free(channel);

    write(STDOUT_FILENO, "\033[r", 3);
    return Result<int>::Ok(job_finished ? exit_status : -1);
}
