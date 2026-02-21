// Experiment: Can we open a second shell channel without triggering Duo?
//
// Theory: Duo is triggered per-session (authentication), not per-channel.
//         Once authenticated, opening additional channels on the same session
//         should NOT trigger Duo because authentication already happened.
//
// Test plan:
//   1. Connect + authenticate (one Duo push)
//   2. Open primary shell channel (PTY + shell) — this is what tccp does today
//   3. Run a command on the primary channel to confirm it works
//   4. Open a SECOND shell channel (PTY + shell) on the same session
//   5. Run a command on the second channel
//   6. If step 4-5 work without a second Duo push → we can use dual channels
//
// Usage: ./dual_shell_test
//   Reads credentials from tccp's credential store.

#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/types.hpp>
#include <ssh/session.hpp>
#include <ssh/shell_channel.hpp>
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <libssh2.h>
#include <fmt/format.h>
#include <iostream>
#include <chrono>
#include <thread>

static void log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 100000;
    fmt::print("[{:05d}] {}\n", ms, msg);
}

// Open a new shell channel on an existing session (PTY + shell)
// Returns nullptr on failure.
static LIBSSH2_CHANNEL* open_shell_channel(
    LIBSSH2_SESSION* session,
    std::shared_ptr<std::mutex> io_mutex,
    const std::string& label)
{
    log(fmt::format("{}: Opening new session channel...", label));

    LIBSSH2_CHANNEL* ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            ch = libssh2_channel_open_session(session);
            if (!ch && libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
                char* errmsg = nullptr;
                libssh2_session_last_error(session, &errmsg, nullptr, 0);
                log(fmt::format("{}: Failed to open channel: {}", label, errmsg ? errmsg : "unknown"));
                return nullptr;
            }
        }
        if (ch) break;
        platform::sleep_ms(10);
    }

    if (!ch) {
        log(fmt::format("{}: Timed out opening channel", label));
        return nullptr;
    }

    log(fmt::format("{}: Channel opened, requesting PTY...", label));

    int tw = platform::term_width();
    int th = platform::term_height();
    int ret;
    while ((ret = libssh2_channel_request_pty_ex(
                ch, "xterm", 5, nullptr, 0, tw, th, 0, 0)) == LIBSSH2_ERROR_EAGAIN) {
        platform::sleep_ms(10);
    }
    if (ret != 0) {
        log(fmt::format("{}: PTY request failed (ret={})", label, ret));
        libssh2_channel_free(ch);
        return nullptr;
    }

    log(fmt::format("{}: PTY granted, requesting shell...", label));

    while ((ret = libssh2_channel_shell(ch)) == LIBSSH2_ERROR_EAGAIN) {
        platform::sleep_ms(10);
    }
    if (ret != 0) {
        log(fmt::format("{}: Shell request failed (ret={})", label, ret));
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        return nullptr;
    }

    log(fmt::format("{}: Shell channel ready!", label));
    return ch;
}

// Run a simple command on a ShellChannel and print the result
static bool test_channel(ShellChannel& ch, const std::string& label) {
    log(fmt::format("{}: Running 'hostname'...", label));
    auto result = ch.run("hostname", 10);
    if (result.exit_code == 0) {
        log(fmt::format("{}: hostname = '{}'", label, result.stdout_data));
        return true;
    } else {
        log(fmt::format("{}: FAILED (exit={}, err='{}')", label, result.exit_code, result.stderr_data));
        return false;
    }
}

int main() {
    log("=== Dual Shell Channel Experiment ===");
    log("");

    // Load config + credentials
    auto load_result = Config::load_global();
    if (load_result.is_err()) {
        log("ERROR: " + load_result.error);
        log("Make sure you've run 'tccp setup' first.");
        return 1;
    }
    Config config = load_result.value;

    auto& creds = CredentialManager::instance();
    auto user_result = creds.get("user");
    auto pass_result = creds.get("password");
    if (user_result.is_err() || pass_result.is_err()) {
        log("ERROR: No credentials. Run 'tccp setup' first.");
        return 1;
    }

    // Connect to DTN
    SessionTarget target;
    target.host = config.dtn().host;
    target.user = user_result.value;
    target.password = pass_result.value;
    target.timeout = 30;
    target.ssh_key_path = config.dtn().ssh_key_path;
    target.use_duo = true;
    target.auto_duo = config.duo_auto().has_value();

    SessionManager session(target);

    log("Connecting to " + target.host + " (expect ONE Duo push)...");
    auto conn_result = session.establish([](const std::string& msg) {
        log("  " + msg);
    });

    if (conn_result.failed()) {
        log("ERROR: Connection failed: " + conn_result.stderr_data);
        return 1;
    }

    log("");
    log("=== Connected! Primary channel is ready. ===");
    log("");

    auto io_mutex = session.io_mutex();
    int sock = session.get_socket();

    // Test 1: Primary channel (already opened by SessionManager)
    {
        log("--- Test 1: Primary shell channel ---");
        ShellChannel primary(session.get_channel(), session.get_raw_session(), io_mutex, sock);
        test_channel(primary, "Primary");
        // Release — don't close, SessionManager owns it
        primary = ShellChannel(nullptr, nullptr, nullptr, -1);
        log("");
    }

    // Test 2: Open a SECOND shell channel
    {
        log("--- Test 2: Second shell channel (should NOT trigger Duo) ---");
        log("If you get a Duo push now, the theory is WRONG.");
        log("Waiting 3s so you can watch for a push...");
        std::this_thread::sleep_for(std::chrono::seconds(3));

        auto* ch2 = open_shell_channel(session.get_raw_session(), io_mutex, "Channel2");
        if (!ch2) {
            log("RESULT: Could not open second channel. Duo may have blocked it.");
            log("Check if you got a Duo push.");
            return 1;
        }

        ShellChannel second(ch2, session.get_raw_session(), io_mutex, sock);
        bool ok = test_channel(second, "Channel2");

        if (ok) {
            log("");
            log("=== SUCCESS: Second shell channel works! ===");
            log("Did you get a second Duo push? If NO → we can safely use multiple channels.");
        } else {
            log("");
            log("=== PARTIAL: Channel opened but command failed. ===");
        }

        log("");
    }

    // Test 3: Open a THIRD channel — exec (no PTY) for comparison
    {
        log("--- Test 3: Exec channel (no PTY, for comparison) ---");

        LIBSSH2_CHANNEL* ch3 = nullptr;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard<std::mutex> lock(*io_mutex);
            ch3 = libssh2_channel_open_session(session.get_raw_session());
            if (ch3) break;
            if (libssh2_session_last_errno(session.get_raw_session()) != LIBSSH2_ERROR_EAGAIN) break;
            platform::sleep_ms(10);
        }

        if (ch3) {
            int ret;
            while ((ret = libssh2_channel_exec(ch3, "hostname")) == LIBSSH2_ERROR_EAGAIN) {
                platform::sleep_ms(10);
            }
            if (ret == 0) {
                char buf[256];
                std::string output;
                for (;;) {
                    int n;
                    {
                        std::lock_guard<std::mutex> lock(*io_mutex);
                        n = libssh2_channel_read(ch3, buf, sizeof(buf) - 1);
                    }
                    if (n > 0) { output.append(buf, n); continue; }
                    if (n == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(10); continue; }
                    break;
                }
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                    output.pop_back();
                log(fmt::format("Exec: hostname = '{}'", output));
            } else {
                log(fmt::format("Exec: exec failed (ret={})", ret));
            }
            {
                std::lock_guard<std::mutex> lock(*io_mutex);
                libssh2_channel_close(ch3);
                libssh2_channel_free(ch3);
            }
        } else {
            log("Exec: Could not open channel");
        }
    }

    log("");
    log("=== Experiment complete ===");
    log("Summary:");
    log("  - If you got exactly ONE Duo push → multiple channels are safe");
    log("  - If you got TWO or more → each channel triggers Duo");
    log("  - If channel2 failed to open → server may reject additional channels");

    session.close();
    return 0;
}
