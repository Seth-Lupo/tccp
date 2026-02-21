// Experiment: Tunnel to login node port 22, get TWO shells without extra Duo.
//
// Strategy:
//   1. Connect to DTN (one Duo push) — Shell 1 on primary channel
//   2. direct-tcpip tunnel to login_node:22
//   3. New libssh2 session over tunnel, password auth
//   4. Try: exec, shell, PTY+shell on login node
//   5. If any work without Duo → Shell 2
//
// Login nodes may have different PAM config than DTN.

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

static void interactive_shell(LIBSSH2_CHANNEL* ch,
                              std::shared_ptr<std::mutex> io_mutex,
                              int sock) {
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

        if (fds[0].revents & POLLIN) {
            int n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
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

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
}

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

// Try to open a shell channel on an inner session.
// Tries multiple strategies, returns the first that works without Duo.
static LIBSSH2_CHANNEL* try_open_inner_shell(LIBSSH2_SESSION* inner, const char* label) {
    LIBSSH2_CHANNEL* ch = nullptr;
    int rc;

    // Strategy A: exec /bin/bash (no PTY, no shell request)
    log(fmt::format("{}: Strategy A — exec /bin/bash (no PTY)...", label));
    ch = libssh2_channel_open_session(inner);
    if (ch) {
        rc = libssh2_channel_exec(ch, "/bin/bash");
        if (rc == 0) {
            log(fmt::format("{}: Strategy A succeeded!", label));
            return ch;
        }
        log(fmt::format("{}: exec failed (rc={})", label, rc));
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
    }

    // Strategy B: PTY + exec /bin/bash (PTY but no shell request)
    log(fmt::format("{}: Strategy B — PTY + exec /bin/bash...", label));
    ch = libssh2_channel_open_session(inner);
    if (ch) {
        rc = libssh2_channel_request_pty_ex(ch, "xterm", 5, nullptr, 0,
                                            platform::term_width(), platform::term_height(), 0, 0);
        if (rc != 0) { log(fmt::format("{}: PTY failed", label)); }
        rc = libssh2_channel_exec(ch, "/bin/bash");
        if (rc == 0) {
            log(fmt::format("{}: Strategy B succeeded!", label));
            return ch;
        }
        log(fmt::format("{}: exec failed (rc={})", label, rc));
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
    }

    // Strategy C: PTY + shell (the one that triggered Duo on DTN)
    log(fmt::format("{}: Strategy C — PTY + shell (may trigger Duo)...", label));
    ch = libssh2_channel_open_session(inner);
    if (ch) {
        rc = libssh2_channel_request_pty_ex(ch, "xterm", 5, nullptr, 0,
                                            platform::term_width(), platform::term_height(), 0, 0);
        if (rc != 0) { log(fmt::format("{}: PTY failed", label)); }
        rc = libssh2_channel_shell(ch);
        if (rc == 0) {
            log(fmt::format("{}: Strategy C succeeded (check if Duo fired!)", label));
            return ch;
        }
        log(fmt::format("{}: shell failed (rc={})", label, rc));
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
    }

    return nullptr;
}

int main() {
    log("=== Tunnel-to-Login-Node Experiment ===");
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
    std::string login_host = config.login().host;

    log(fmt::format("DTN: {}", config.dtn().host));
    log(fmt::format("Login: {}", login_host));
    log("");

    // Step 1: Connect to DTN (one Duo push)
    SessionTarget target;
    target.host = config.dtn().host;
    target.user = username;
    target.password = password;
    target.timeout = 30;
    target.ssh_key_path = config.dtn().ssh_key_path;
    target.use_duo = true;
    target.auto_duo = config.duo_auto().has_value();

    SessionManager session(target);
    log("Step 1: Connecting to DTN (expect ONE Duo push)...");
    auto conn_result = session.establish([](const std::string& msg) { log("  " + msg); });
    if (conn_result.failed()) { log("ERROR: " + conn_result.stderr_data); return 1; }
    log("Connected to DTN!");
    log("");

    auto io_mutex = session.io_mutex();
    int outer_sock = session.get_socket();

    // Step 2: Tunnel to login node:22
    log(fmt::format("Step 2: Opening tunnel to {}:22...", login_host));

    LIBSSH2_CHANNEL* tunnel_ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            tunnel_ch = libssh2_channel_direct_tcpip(
                session.get_raw_session(), login_host.c_str(), 22);
            if (!tunnel_ch &&
                libssh2_session_last_errno(session.get_raw_session()) != LIBSSH2_ERROR_EAGAIN)
                break;
        }
        if (tunnel_ch) break;
        platform::sleep_ms(10);
    }
    if (!tunnel_ch) { log("ERROR: Tunnel failed"); return 1; }
    log("Tunnel open!");

    // Socketpair + pump
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { log("ERROR: socketpair"); return 1; }

    std::atomic<bool> stop_pump{false};
    std::thread pump(pump_thread, tunnel_ch, io_mutex, outer_sock, sv[0], std::ref(stop_pump));

    // Step 3: Inner SSH session to login node
    log("Step 3: Inner SSH handshake to login node...");

    LIBSSH2_SESSION* inner = libssh2_session_init();
    libssh2_session_set_blocking(inner, 1);

    int rc = libssh2_session_handshake(inner, sv[1]);
    if (rc != 0) {
        char* errmsg = nullptr;
        libssh2_session_last_error(inner, &errmsg, nullptr, 0);
        log(fmt::format("ERROR: Handshake failed: {}", errmsg ? errmsg : "unknown"));
        stop_pump = true; pump.join();
        close(sv[0]); close(sv[1]);
        session.close();
        return 1;
    }
    log("  Handshake OK!");

    char* auth_list = libssh2_userauth_list(inner, username.c_str(), username.length());
    if (auth_list) log(fmt::format("  Auth methods: {}", auth_list));

    // Try password auth
    log("Step 4: Password auth on login node...");
    rc = libssh2_userauth_password(inner, username.c_str(), password.c_str());
    if (rc != 0) {
        char* errmsg = nullptr;
        libssh2_session_last_error(inner, &errmsg, nullptr, 0);
        log(fmt::format("  Password auth failed: {}", errmsg ? errmsg : "unknown"));
        libssh2_session_disconnect(inner, "done");
        libssh2_session_free(inner);
        stop_pump = true; pump.join();
        close(sv[0]); close(sv[1]);
        session.close();
        return 1;
    }
    log("  Password auth succeeded (no Duo so far)!");
    log("");

    // Step 5: Try opening a shell
    log("Step 5: Opening shell on login node (trying multiple strategies)...");
    log("  Watch for Duo pushes...");
    log("");

    LIBSSH2_CHANNEL* inner_ch = try_open_inner_shell(inner, "Login");
    if (!inner_ch) {
        log("ERROR: All strategies failed");
        libssh2_session_disconnect(inner, "done");
        libssh2_session_free(inner);
        stop_pump = true; pump.join();
        close(sv[0]); close(sv[1]);
        session.close();
        return 1;
    }

    log("");
    log("=== SHELL 1 (DTN primary channel) ===");
    log("Press Ctrl+D to switch to Shell 2.");
    log("");

    interactive_shell(session.get_channel(), io_mutex, outer_sock);

    log("");
    log("Shell 1 closed.");
    log("");
    log("=== SHELL 2 (login node via tunnel) ===");
    log("Press Ctrl+D to exit.");
    log("");

    interactive_shell_inner(inner_ch, inner, sv[1]);

    log("");
    log("Shell 2 closed.");
    log("");
    log("=== Results ===");
    log("If you got exactly ONE Duo push → login node tunnel works!");
    log("Two independent shells, one Duo push.");

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
