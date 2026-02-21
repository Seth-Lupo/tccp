// Experiment: Two independent interactive shells through one Duo push.
//
// Architecture:
//   [outer session] --direct-tcpip--> 127.0.0.1:22 --socketpair--> [inner session]
//
// Shell 1: Primary channel on outer session (has Kerberos)
// Shell 2: Shell channel on inner session (via tunnel, no Duo)

#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/types.hpp>
#include <ssh/session.hpp>
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#include <fmt/format.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <termios.h>

static void log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 100000;
    fmt::print("[{:05d}] {}\n", ms, msg);
}

// Pump thread: tunnel channel <-> local socket
static void pump_thread(LIBSSH2_CHANNEL* tunnel_ch,
                        std::shared_ptr<std::mutex> io_mutex,
                        int outer_sock,
                        int local_sock,
                        std::atomic<bool>& stop) {
    char buf[16384];
    while (!stop.load()) {
        struct pollfd fds[2];
        fds[0] = {outer_sock, POLLIN, 0};
        fds[1] = {local_sock, POLLIN, 0};
        poll(fds, 2, 50);

        if (fds[0].revents & POLLIN) {
            for (;;) {
                int n;
                {
                    std::lock_guard<std::mutex> lock(*io_mutex);
                    n = libssh2_channel_read(tunnel_ch, buf, sizeof(buf));
                }
                if (n > 0) {
                    int sent = 0;
                    while (sent < n) {
                        int w = ::write(local_sock, buf + sent, n - sent);
                        if (w <= 0) break;
                        sent += w;
                    }
                } else break;
            }
        }

        if (fds[1].revents & POLLIN) {
            int n = ::read(local_sock, buf, sizeof(buf));
            if (n > 0) {
                int sent = 0;
                while (sent < n) {
                    int w;
                    {
                        std::lock_guard<std::mutex> lock(*io_mutex);
                        w = libssh2_channel_write(tunnel_ch, buf + sent, n - sent);
                    }
                    if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
                    if (w < 0) break;
                    sent += w;
                }
            }
        }
    }
}

// Interactive relay: stdin → channel, channel → stdout
// Returns when user presses Ctrl+D or channel closes.
static void interactive_shell(LIBSSH2_CHANNEL* ch,
                              std::shared_ptr<std::mutex> io_mutex,
                              int sock) {
    // Raw terminal mode
    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char buf[16384];
    bool running = true;

    while (running) {
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {sock, POLLIN, 0};
        poll(fds, 2, 100);

        // stdin → channel
        if (fds[0].revents & POLLIN) {
            int n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            // Ctrl+D detection (0x04)
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x04) { running = false; break; }
            }
            if (!running) break;
            int sent = 0;
            while (sent < n) {
                int w;
                {
                    std::lock_guard<std::mutex> lock(*io_mutex);
                    w = libssh2_channel_write(ch, buf + sent, n - sent);
                }
                if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
                if (w < 0) { running = false; break; }
                sent += w;
            }
        }

        // channel → stdout
        if (fds[1].revents & POLLIN) {
            for (;;) {
                int n;
                {
                    std::lock_guard<std::mutex> lock(*io_mutex);
                    n = libssh2_channel_read(ch, buf, sizeof(buf));
                }
                if (n > 0) {
                    ::write(STDOUT_FILENO, buf, n);
                } else break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            if (libssh2_channel_eof(ch)) running = false;
        }
    }

    // Restore terminal
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
}

// Interactive relay for inner session (uses inner session's socket = sv[1])
// The pump thread handles the actual network I/O.
static void interactive_shell_inner(LIBSSH2_CHANNEL* ch,
                                    LIBSSH2_SESSION* sess,
                                    int inner_sock) {
    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char buf[16384];
    bool running = true;

    // Make non-blocking for polling
    libssh2_session_set_blocking(sess, 0);

    while (running) {
        struct pollfd fds[2];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        fds[1] = {inner_sock, POLLIN, 0};
        poll(fds, 2, 100);

        if (fds[0].revents & POLLIN) {
            int n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0x04) { running = false; break; }
            }
            if (!running) break;
            int sent = 0;
            while (sent < n) {
                int w = libssh2_channel_write(ch, buf + sent, n - sent);
                if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
                if (w < 0) { running = false; break; }
                sent += w;
            }
        }

        if (fds[1].revents & POLLIN) {
            for (;;) {
                int n = libssh2_channel_read(ch, buf, sizeof(buf));
                if (n > 0) {
                    ::write(STDOUT_FILENO, buf, n);
                } else break;
            }
        }

        if (libssh2_channel_eof(ch)) running = false;
    }

    libssh2_session_set_blocking(sess, 1);
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
}

int main() {
    log("=== Dual Interactive Shell Experiment ===");
    log("");

    auto load_result = Config::load_global();
    if (load_result.is_err()) { log("ERROR: " + load_result.error); return 1; }
    Config config = load_result.value;

    auto& creds = CredentialManager::instance();
    auto user_result = creds.get("user");
    auto pass_result = creds.get("password");
    if (user_result.is_err() || pass_result.is_err()) { log("ERROR: No creds"); return 1; }
    std::string username = user_result.value;
    std::string password = pass_result.value;

    // Step 1: Connect (one Duo push)
    SessionTarget target;
    target.host = config.dtn().host;
    target.user = username;
    target.password = password;
    target.timeout = 30;
    target.ssh_key_path = config.dtn().ssh_key_path;
    target.use_duo = true;
    target.auto_duo = config.duo_auto().has_value();

    SessionManager session(target);
    log("Connecting (expect ONE Duo push)...");
    auto conn_result = session.establish([](const std::string& msg) { log("  " + msg); });
    if (conn_result.failed()) { log("ERROR: " + conn_result.stderr_data); return 1; }
    log("Connected!");
    log("");

    auto io_mutex = session.io_mutex();
    int outer_sock = session.get_socket();

    // Step 2: Tunnel + inner session
    log("Opening tunnel to 127.0.0.1:22...");
    LIBSSH2_CHANNEL* tunnel_ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            tunnel_ch = libssh2_channel_direct_tcpip(session.get_raw_session(), "127.0.0.1", 22);
            if (!tunnel_ch && libssh2_session_last_errno(session.get_raw_session()) != LIBSSH2_ERROR_EAGAIN)
                break;
        }
        if (tunnel_ch) break;
        platform::sleep_ms(10);
    }
    if (!tunnel_ch) { log("ERROR: Tunnel failed"); return 1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { log("ERROR: socketpair"); return 1; }

    std::atomic<bool> stop_pump{false};
    std::thread pump(pump_thread, tunnel_ch, io_mutex, outer_sock, sv[0], std::ref(stop_pump));

    LIBSSH2_SESSION* inner = libssh2_session_init();
    libssh2_session_set_blocking(inner, 1);
    if (libssh2_session_handshake(inner, sv[1]) != 0) { log("ERROR: inner handshake"); return 1; }
    if (libssh2_userauth_password(inner, username.c_str(), password.c_str()) != 0) {
        log("ERROR: inner auth"); return 1;
    }

    log("Inner session authenticated (no Duo!)");
    log("");

    // === Interactive Shell 1: Primary channel ===
    log("=== SHELL 1 (primary channel — has Kerberos) ===");
    log("Press Ctrl+D to exit this shell and try shell 2.");
    log("");

    interactive_shell(session.get_channel(), io_mutex, outer_sock);

    log("");
    log("Shell 1 closed.");
    log("");

    // === Shell 2: Try exec /bin/bash (no PTY, no shell request) ===
    log("=== SHELL 2: exec '/bin/bash -i' (no PTY, no shell request) ===");
    log("If no Duo push appears, this is the winner.");
    log("Waiting 3s to watch for Duo...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    LIBSSH2_CHANNEL* inner_ch = libssh2_channel_open_session(inner);
    if (!inner_ch) { log("ERROR: inner channel open failed"); return 1; }

    // NO pty request. NO shell request. Just exec.
    int rc = libssh2_channel_exec(inner_ch, "/bin/bash -i");
    if (rc != 0) {
        log(fmt::format("ERROR: exec failed (rc={})", rc));
        libssh2_channel_free(inner_ch);
        // Fallback: try with PTY but exec instead of shell
        log("");
        log("=== SHELL 2b: PTY + exec '/bin/bash' (no shell request) ===");
        log("Waiting 3s...");
        std::this_thread::sleep_for(std::chrono::seconds(3));

        inner_ch = libssh2_channel_open_session(inner);
        if (!inner_ch) { log("ERROR: channel open failed"); return 1; }
        libssh2_channel_request_pty_ex(inner_ch, "xterm", 5, nullptr, 0,
                                       platform::term_width(), platform::term_height(), 0, 0);
        rc = libssh2_channel_exec(inner_ch, "/bin/bash");
        if (rc != 0) { log("ERROR: exec failed too"); return 1; }
    }

    log("Shell 2 launched! Press Ctrl+D to exit.");
    log("");

    interactive_shell_inner(inner_ch, inner, sv[1]);

    log("");
    log("Shell 2 closed.");
    log("");
    log("=== Experiment complete — two shells, one Duo push! ===");

    // Cleanup
    libssh2_channel_close(inner_ch);
    libssh2_channel_free(inner_ch);
    libssh2_session_disconnect(inner, "done");
    libssh2_session_free(inner);
    stop_pump = true;
    pump.join();
    close(sv[0]); close(sv[1]);
    {
        std::lock_guard<std::mutex> lock(*io_mutex);
        libssh2_channel_close(tunnel_ch);
        libssh2_channel_free(tunnel_ch);
    }
    session.close();

    return 0;
}
