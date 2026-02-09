#include "job_view.hpp"
#include <iostream>
#include <libssh2.h>
#include <fmt/format.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <chrono>
#include <cstdio>
#include <cstring>

// ── Constants ───────────────────────────────────────────────

static constexpr int HEADER_ROWS = 2;

// ── SIGWINCH ────────────────────────────────────────────────

static volatile sig_atomic_t g_need_resize = 0;

static void handle_sigwinch(int) { g_need_resize = 1; }

// ── Output filters ──────────────────────────────────────────

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
    out += "\033[48;2;74;74;74m\033[38;2;150;150;150m";
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

    std::string cmd;
    if (skip_remote_replay) {
        cmd = fmt::format(
            "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
            "\"while [ ! -e {} ]; do sleep 1; done; "
            "\\$HOME/tccp/bin/dtach -a {} -r none\"",
            compute_node_, dtach_socket_, dtach_socket_);
    } else {
        cmd = fmt::format(
            "ssh -t -o StrictHostKeyChecking=no -o LogLevel=ERROR {} "
            "\"while [ ! -e {} ]; do sleep 1; done; "
            "tail -c 65536 {} 2>/dev/null | perl -pe 's/\\e\\[[\\d;]*[HJK]//g'; "
            "\\$HOME/tccp/bin/dtach -a {} -r none\"",
            compute_node_, dtach_socket_, log_path, dtach_socket_);
    }

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
    auto last_header = std::chrono::steady_clock::now();
    bool remote_eof = false;
    bool user_detached = false;
    bool write_error = false;
    int consecutive_newlines = 0;

    while (!write_error) {
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

        // stdin → remote
        if (fds[0].revents & POLLIN) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x1c) user_detached = true;
            }

            int offset = 0;
            while (offset < n) {
                int w = libssh2_channel_write(channel, buf + offset, n - offset);
                if (w == LIBSSH2_ERROR_EAGAIN) { usleep(1000); continue; }
                if (w < 0) { write_error = true; break; }
                offset += w;
            }
        }

        // remote → stdout + capture
        if (fds[1].revents & POLLIN) {
            bool got_output = false;
            for (;;) {
                int n = libssh2_channel_read(channel, buf, sizeof(buf));
                if (n <= 0) break;

                std::string display = filter_for_display(buf, n, consecutive_newlines);
                if (!display.empty()) {
                    write(STDOUT_FILENO, display.data(), display.size());
                    if (capture) {
                        std::string filtered = filter_for_capture(
                            display.data(), display.size());
                        fwrite(filtered.data(), 1, filtered.size(), capture);
                        fflush(capture);
                    }
                }
                got_output = true;
            }

            if (got_output) {
                auto now = std::chrono::steady_clock::now();
                if ((now - last_header) >= std::chrono::milliseconds(500)) {
                    draw_header();
                    last_header = now;
                }
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
        // Drain remaining junk
        for (;;) {
            if (libssh2_channel_read(channel, buf, sizeof(buf)) <= 0) break;
        }

        // Position exit prompt within the scroll region (above header)
        struct winsize ews;
        int erows = 24;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ews) == 0 && ews.ws_row > 0)
            erows = ews.ws_row;
        int prompt_row = std::max(1, erows - HEADER_ROWS);

        std::string exit_msg;
        if (exit_status == 0)
            exit_msg = fmt::format("\033[{};1H\033[2m[exit 0]\033[0m  \033[2mPress Enter to leave...\033[0m", prompt_row);
        else
            exit_msg = fmt::format("\033[{};1H\033[91m[exit {}]\033[0m  \033[2mPress Enter to leave...\033[0m", prompt_row, exit_status);
        write(STDOUT_FILENO, exit_msg.data(), exit_msg.size());
        draw_header();

        for (;;) {
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0) break;
            if (c == '\r' || c == '\n') break;
        }
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
