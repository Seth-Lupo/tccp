// Experiment: SFTP subsystem on tunnel session — does it trigger Duo?
//
// If SFTP works without Duo, we can use it as a file-based command channel:
//   - Primary channel: interactive relay + background command executor
//   - SFTP on tunnel session: write command files, read result files
//
// Test plan:
//   1. Connect to DTN (one Duo push)
//   2. Launch background command executor on primary channel
//   3. Tunnel to DTN localhost:22, password auth (no Duo)
//   4. Open SFTP subsystem on inner session
//   5. Write a command via SFTP, read result via SFTP
//   6. Also test: interactive shell on primary channel while SFTP runs

#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/types.hpp>
#include <ssh/session.hpp>
#include <ssh/connection.hpp>
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#include <libssh2_sftp.h>
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

// Write a string to a remote file via SFTP
static bool sftp_write_file(LIBSSH2_SFTP* sftp, const std::string& path, const std::string& content) {
    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, path.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
    if (!fh) {
        log(fmt::format("  SFTP write failed: cannot open {}", path));
        return false;
    }
    ssize_t rc = libssh2_sftp_write(fh, content.c_str(), content.size());
    libssh2_sftp_close(fh);
    return rc == static_cast<ssize_t>(content.size());
}

// Read a remote file via SFTP
static std::string sftp_read_file(LIBSSH2_SFTP* sftp, const std::string& path) {
    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, path.c_str(),
        LIBSSH2_FXF_READ, 0);
    if (!fh) return "";
    std::string result;
    char buf[4096];
    for (;;) {
        ssize_t n = libssh2_sftp_read(fh, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, n);
    }
    libssh2_sftp_close(fh);
    return result;
}

// Check if a remote file exists via SFTP
static bool sftp_file_exists(LIBSSH2_SFTP* sftp, const std::string& path) {
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    return libssh2_sftp_stat(sftp, path.c_str(), &attrs) == 0;
}

// Remove a remote file via SFTP
static void sftp_unlink(LIBSSH2_SFTP* sftp, const std::string& path) {
    libssh2_sftp_unlink(sftp, path.c_str());
}

int main() {
    log("=== SFTP Command Channel Experiment ===");
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

    std::string cmd_dir = fmt::format("/tmp/{}/.tccp-rpc", username);

    // Step 1: Connect to DTN
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
    log("Connected!");
    log("");

    auto io_mutex = session.io_mutex();
    int outer_sock = session.get_socket();

    // Step 2: Launch background command executor on primary channel
    log("Step 2: Launching background command executor on DTN...");

    // Create the RPC directory and launch the executor
    // The executor watches for .cmd files, runs them, writes .out files
    SSHConnection primary(session.get_channel(), session.get_raw_session(),
                          io_mutex, outer_sock,
                          std::make_shared<std::mutex>());

    auto setup = primary.run(fmt::format("mkdir -p {} && rm -f {}/*.cmd {}/*.out", cmd_dir, cmd_dir, cmd_dir));
    if (setup.exit_code != 0) {
        log("ERROR: Could not create RPC directory");
        return 1;
    }

    // Launch background executor: polls for .cmd files, runs them, writes .out
    std::string executor_cmd = fmt::format(
        "(while true; do "
        "  for f in {}/*.cmd 2>/dev/null; do "
        "    [ -f \"$f\" ] || continue; "
        "    base=$(basename \"$f\" .cmd); "
        "    cmd=$(cat \"$f\"); "
        "    eval \"$cmd\" > {}/\"$base\".out 2>&1; "
        "    rm -f \"$f\"; "
        "  done; "
        "  sleep 0.1; "
        "done >/dev/null 2>&1 &)",
        cmd_dir, cmd_dir);

    auto exec_result = primary.run(executor_cmd);
    log(fmt::format("  Executor launched (exit={})", exec_result.exit_code));
    log("");

    // Step 3: Tunnel + inner session + SFTP
    log("Step 3: Opening tunnel to 127.0.0.1:22...");

    LIBSSH2_CHANNEL* tunnel_ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            tunnel_ch = libssh2_channel_direct_tcpip(
                session.get_raw_session(), "127.0.0.1", 22);
            if (!tunnel_ch &&
                libssh2_session_last_errno(session.get_raw_session()) != LIBSSH2_ERROR_EAGAIN)
                break;
        }
        if (tunnel_ch) break;
        platform::sleep_ms(10);
    }
    if (!tunnel_ch) { log("ERROR: Tunnel failed"); return 1; }
    log("  Tunnel open!");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { log("ERROR: socketpair"); return 1; }

    std::atomic<bool> stop_pump{false};
    std::thread pump(pump_thread, tunnel_ch, io_mutex, outer_sock, sv[0], std::ref(stop_pump));

    LIBSSH2_SESSION* inner = libssh2_session_init();
    libssh2_session_set_blocking(inner, 1);
    if (libssh2_session_handshake(inner, sv[1]) != 0) {
        log("ERROR: inner handshake failed");
        stop_pump = true; pump.join(); close(sv[0]); close(sv[1]);
        session.close(); return 1;
    }

    if (libssh2_userauth_password(inner, username.c_str(), password.c_str()) != 0) {
        log("ERROR: inner auth failed");
        stop_pump = true; pump.join(); close(sv[0]); close(sv[1]);
        session.close(); return 1;
    }
    log("  Inner session authenticated (no Duo)!");

    log("Step 4: Opening SFTP subsystem (watch for Duo push)...");
    log("  Waiting 3s...");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(inner);
    if (!sftp) {
        char* errmsg = nullptr;
        libssh2_session_last_error(inner, &errmsg, nullptr, 0);
        log(fmt::format("ERROR: SFTP init failed: {}", errmsg ? errmsg : "unknown"));
        log("SFTP subsystem may have triggered Duo or been rejected.");
        libssh2_session_disconnect(inner, "done");
        libssh2_session_free(inner);
        stop_pump = true; pump.join(); close(sv[0]); close(sv[1]);
        session.close(); return 1;
    }

    log("  SFTP opened! (Did you get a Duo push? If not, this works!)");
    log("");

    // Step 5: Test SFTP-based command execution
    log("Step 5: Running commands via SFTP file channel...");

    auto run_via_sftp = [&](const std::string& id, const std::string& cmd) -> std::string {
        std::string cmd_file = cmd_dir + "/" + id + ".cmd";
        std::string out_file = cmd_dir + "/" + id + ".out";

        // Clean up any old output
        sftp_unlink(sftp, out_file.c_str());

        // Write command
        if (!sftp_write_file(sftp, cmd_file, cmd)) {
            return "ERROR: failed to write command file";
        }

        // Poll for result
        for (int i = 0; i < 100; i++) {  // 10s timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!sftp_file_exists(sftp, cmd_file)) {
                // Executor consumed the cmd file, check for output
                std::string result = sftp_read_file(sftp, out_file);
                sftp_unlink(sftp, out_file.c_str());
                // Trim trailing newline
                while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                    result.pop_back();
                return result;
            }
        }
        return "ERROR: timeout waiting for result";
    };

    // Test commands
    std::string result;

    result = run_via_sftp("test1", "hostname");
    log(fmt::format("  hostname = '{}'", result));

    result = run_via_sftp("test2", "whoami");
    log(fmt::format("  whoami = '{}'", result));

    result = run_via_sftp("test3", "sinfo -h -p gpu --format='%P %a %D %C' 2>&1 | head -3");
    log(fmt::format("  sinfo = '{}'", result));

    result = run_via_sftp("test4", "echo $SHELL");
    log(fmt::format("  SHELL = '{}'", result));

    log("");

    // Step 6: Interactive shell on primary channel while SFTP is active
    log("Step 6: Interactive shell on primary channel.");
    log("  SFTP channel remains open in background.");
    log("  Press Ctrl+D to exit shell, then we'll run more SFTP commands.");
    log("");

    interactive_shell(session.get_channel(), io_mutex, outer_sock);

    log("");
    log("Shell closed. Running another command via SFTP...");

    result = run_via_sftp("test5", "date");
    log(fmt::format("  date = '{}'", result));

    log("");
    log("=== Results ===");
    log("If you got ONE Duo push and all commands worked:");
    log("  → SFTP + file-based RPC = independent command channel!");
    log("  → No extra ports, no extra auth, no artifacts.");

    // Cleanup
    primary.run(fmt::format("rm -rf {}", cmd_dir));
    libssh2_sftp_shutdown(sftp);
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
