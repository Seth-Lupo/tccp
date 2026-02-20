#include "shell_relay.hpp"
#include "connection_factory.hpp"
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#ifdef _WIN32
#  include <io.h>
#  define STDIN_FILENO  0
#  define STDOUT_FILENO 1
#  define read  _read
#  define write _write
#else
#  include <unistd.h>
#  include <poll.h>
#endif
#include <algorithm>

static const std::string DONE_MARKER = "__TCCP_DONE__";
// Shell string splitting: DO''NE prevents the marker from appearing in PTY echo.
static const std::string DONE_CMD = " ; echo __TCCP_DO''NE__\n";

// ── Lifecycle ──────────────────────────────────────────────────

ShellRelay::ShellRelay(ConnectionFactory& factory) : factory_(factory) {}

ShellRelay::~ShellRelay() {
    if (started_) stop();
}

bool ShellRelay::start(const std::string& command) {
    ch_ = factory_.primary_channel();
    if (!ch_) return false;

    io_mutex_ = factory_.io_mutex();
    cmd_mutex_ = factory_.primary_cmd_mutex();
    sock_ = factory_.raw_socket();

    cmd_mutex_->lock();  // Hold for entire relay session
    started_ = true;
    skip_echo_ = true;
    done_ = false;
    buf_.clear();

    drain();
    send(command + DONE_CMD);
    return true;
}

std::string ShellRelay::feed(const char* data, int len) {
    if (len <= 0 || done_) return {};

    buf_.append(data, len);

    // Phase 1: skip echoed command line (everything up to first \n)
    if (skip_echo_) {
        auto nl = buf_.find('\n');
        if (nl == std::string::npos) return {};
        buf_ = buf_.substr(nl + 1);
        skip_echo_ = false;
        if (buf_.empty()) return {};
    }

    // Phase 2: check for done marker
    auto dm = buf_.find(DONE_MARKER);
    if (dm != std::string::npos) {
        std::string result = buf_.substr(0, dm);
        buf_.clear();
        done_ = true;
        return result;
    }

    // Only hold back bytes that could be the start of the done marker.
    // Check if the tail of buf_ matches a prefix of DONE_MARKER.
    size_t hold = 0;
    for (size_t i = 1; i <= (std::min)(buf_.size(), DONE_MARKER.size()); i++) {
        if (buf_.compare(buf_.size() - i, i, DONE_MARKER, 0, i) == 0)
            hold = i;
    }

    if (hold >= buf_.size()) return {};
    std::string result = buf_.substr(0, buf_.size() - hold);
    buf_ = buf_.substr(buf_.size() - hold);
    return result;
}

bool ShellRelay::done() const { return done_; }

void ShellRelay::stop() {
    if (!started_) return;

    char tmp[4096];

    if (!done_) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            char ctrl_c = 0x03;
            libssh2_channel_write(ch_, &ctrl_c, 1);
        }
        platform::sleep_ms(100);
    }

    // Drain remaining data (prompt, post-marker text)
    for (int i = 0; i < 100; i++) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(ch_, tmp, sizeof(tmp));
        }
        if (n <= 0) break;
    }

    started_ = false;
    ch_ = nullptr;
    cmd_mutex_->unlock();
}

LIBSSH2_CHANNEL* ShellRelay::channel() const { return ch_; }
std::shared_ptr<std::mutex> ShellRelay::io_mutex() const { return io_mutex_; }
int ShellRelay::socket() const { return sock_; }

// ── Simple interface ───────────────────────────────────────────

static volatile int g_relay_resize = 0;

void ShellRelay::run(const std::string& command) {
    if (!start(command)) return;

    platform::on_terminal_resize([] { g_relay_resize = 1; });
    platform::RawModeGuard raw(platform::RawModeGuard::kFullRaw);

    // Set PTY to actual terminal size before entering the loop
    {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        libssh2_channel_request_pty_size(
            ch_, platform::term_width(), platform::term_height());
    }

    char rbuf[16384];

    while (!done_) {
        // Propagate terminal resize to remote PTY
        if (g_relay_resize) {
            g_relay_resize = 0;
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_channel_request_pty_size(
                ch_, platform::term_width(), platform::term_height());
        }

        bool stdin_ready = false;
        bool sock_ready = false;
#ifdef _WIN32
        stdin_ready = platform::poll_stdin(0);
        sock_ready = platform::poll_socket(socket(), POLLIN, stdin_ready ? 0 : 100) & POLLIN;
#else
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {socket(), POLLIN, 0};
        poll(fds, 2, 100);
        stdin_ready = fds[0].revents & POLLIN;
        sock_ready = fds[1].revents & POLLIN;
#endif

        // stdin → channel
        if (stdin_ready) {
            int n = read(STDIN_FILENO, rbuf, sizeof(rbuf));
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                int w;
                {
                    std::lock_guard<std::mutex> lock(*io_mutex_);
                    w = libssh2_channel_write(ch_, rbuf + sent, n - sent);
                }
                if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
                if (w < 0) goto out;
                sent += w;
            }
        }

        // channel → stdout
        if (sock_ready) {
            for (;;) {
                int n;
                {
                    std::lock_guard<std::mutex> lock(*io_mutex_);
                    n = libssh2_channel_read(ch_, rbuf, sizeof(rbuf));
                }
                if (n <= 0) break;
                std::string text = feed(rbuf, n);
                if (!text.empty())
                    write(STDOUT_FILENO, text.data(), text.size());
                if (done_) break;
            }
        }

        bool eof;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            eof = libssh2_channel_eof(ch_);
        }
        if (eof) break;
    }

out:
    platform::remove_terminal_resize();
    stop();
}

// ── Helpers ────────────────────────────────────────────────────

void ShellRelay::drain() {
    char tmp[4096];
    for (;;) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            n = libssh2_channel_read(ch_, tmp, sizeof(tmp));
        }
        if (n <= 0) break;
    }
}

void ShellRelay::send(const std::string& text) {
    int total = static_cast<int>(text.size());
    int sent = 0;
    while (sent < total) {
        int w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            w = libssh2_channel_write(ch_, text.c_str() + sent, total - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
        if (w < 0) return;
        sent += w;
    }
}
