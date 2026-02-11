#include "../base_cli.hpp"
#include "../job_view.hpp"
#include "../theme.hpp"
#include <core/credentials.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

// ── Helpers ──────────────────────────────────────────────────

static std::string get_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "";
}

static std::string output_path_for(const std::string& job_id) {
    return "/tmp/tccp_output_" + job_id + ".log";
}

static std::string dtach_sock_for(const std::string& job_id) {
    return "/tmp/tccp_" + job_id + ".sock";
}

// ── Alternate screen + bottom status bar ─────────────────────

static void enter_alt_screen() {
    std::cout << "\033[?1049h\033[H" << std::flush;
}

static void leave_alt_screen() {
    std::cout << "\033[r\033[?1049l" << std::flush;
}

static void setup_content_area() {
    struct winsize ws;
    int rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        rows = ws.ws_row;
    std::cout << fmt::format("\033[1;{}r", rows - 2);
    std::cout << "\033[1;1H" << std::flush;
}

static int term_width() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static int term_rows() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

static void draw_header(const std::string& job_name,
                         const std::string& status,
                         const std::string& slurm_id = "",
                         const std::string& node = "") {
    int w = term_width();
    int h = term_rows();

    std::string l1 = " \033[38;2;62;120;178;1mtccp\033[22;38;2;178;178;178m";
    std::string l1_text = " tccp";
    if (!job_name.empty())  { l1 += " | " + job_name;         l1_text += " | " + job_name; }
    if (!slurm_id.empty())  { l1 += " | SLURM:" + slurm_id;   l1_text += " | SLURM:" + slurm_id; }
    if (!node.empty())      { l1 += " | " + node;              l1_text += " | " + node; }
    int l1_pad = std::max(0, w - static_cast<int>(l1_text.size()));

    std::string l2_left = " " + status;
    std::string l2_right = (status == "INITIALIZING...")
        ? "Ctrl+C cancel  Ctrl+\\ detach "
        : "Ctrl+\\ to detach ";
    int l2_pad = std::max(0, w - static_cast<int>(l2_left.size()) - static_cast<int>(l2_right.size()));

    std::cout << "\0337";

    std::cout << fmt::format("\033[{};1H", h - 1);
    std::cout << "\033[48;2;62;62;62m\033[38;2;178;178;178m";
    std::cout << l1;
    for (int i = 0; i < l1_pad; i++) std::cout << ' ';
    std::cout << "\033[0m";

    std::cout << fmt::format("\033[{};1H", h);
    std::cout << "\033[48;2;74;74;74m\033[38;2;150;150;150m";
    std::cout << l2_left;
    for (int i = 0; i < l2_pad; i++) std::cout << ' ';
    std::cout << "\033[38;2;120;120;120m" << l2_right;
    std::cout << "\033[0m";

    std::cout << "\0338" << std::flush;
}

// Wait up to `ms` milliseconds. Returns true if ESC ESC pressed.
static bool wait_or_detach(int ms) {
    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;
    new_term.c_cc[VTIME] = 0;
    tcflush(STDIN_FILENO, TCIFLUSH);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    bool first_esc = false;
    auto first_esc_time = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        int ret = poll(&pfd, 1, static_cast<int>(std::min(remaining, (long long)100)));

        if (ret > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 27) {
                    auto now = std::chrono::steady_clock::now();
                    if (first_esc && (now - first_esc_time) < std::chrono::milliseconds(500)) {
                        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                        return true;
                    }
                    first_esc = true;
                    first_esc_time = now;
                } else {
                    first_esc = false;
                }
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    return false;
}

// ── Shared: attach to a running job ─────────────────────────

static bool attach_to_job(BaseCLI& cli, TrackedJob* tracked,
                           const std::string& job_name, bool replay,
                           bool in_alt_screen = true) {
    std::string output_path = output_path_for(tracked->job_id);
    tracked->output_file = output_path;

    JobView view(*cli.cluster->get_session(),
                 tracked->compute_node,
                 get_username(),
                 dtach_sock_for(tracked->job_id),
                 job_name,
                 tracked->slurm_id,
                 output_path,
                 tracked->job_id,
                 tracked->canceled);
    auto result = view.attach(replay);
    if (in_alt_screen) leave_alt_screen();

    if (result.is_err()) {
        std::cout << theme::fail("Attach failed: " + result.error);
        return false;
    }

    if (result.value == -2) {
        // User pressed Ctrl+C to cancel job
        std::cout << theme::step("Canceling job...");
        auto cancel_result = cli.jobs->cancel_job_by_id(tracked->job_id, nullptr);
        if (cancel_result.is_ok()) {
            std::cout << theme::ok(fmt::format("Job '{}' canceled", job_name));
        } else {
            std::cout << theme::fail(cancel_result.error);
        }
        return false;
    }

    if (result.value >= 0) {
        tracked->exit_code = result.value;
        tracked->completed = true;
        if (cli.jobs) cli.jobs->on_job_complete(*tracked);
        std::cout << theme::step("Session ended.");
        return true;
    }

    std::cout << theme::step("Detached. Use 'view " + job_name + "' to re-attach.");
    return false;
}

// ── Commands ────────────────────────────────────────────────

static void do_view(BaseCLI& cli, const std::string& arg);  // forward decl

static void do_run(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: run <job-name>");
        return;
    }

    auto* existing = cli.jobs->find_by_name(arg);
    if (existing && !existing->completed && existing->init_error.empty()) {
        if (!existing->init_complete) {
            std::cout << theme::fail(fmt::format("Job '{}' is still initializing. "
                "Use 'view {}' to check progress.", arg, arg));
        } else {
            std::cout << theme::fail(fmt::format("Job '{}' is already running. "
                "Cancel it first or wait for it to finish.", arg));
        }
        return;
    }

    // Start the job - this spawns background init and returns immediately
    auto result = cli.jobs->run(arg, nullptr);

    if (result.is_err()) {
        std::cout << theme::fail(result.error) << "\n";
        return;
    }

    // Immediately attach to see init progress (user can Ctrl+\ to detach)
    do_view(cli, arg);
}

static void do_view(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: view <job-name>");
        return;
    }

    auto* tracked = cli.jobs->find_by_name(arg);
    if (!tracked) {
        std::cout << theme::fail(fmt::format("No tracked job named '{}'", arg));
        return;
    }

    // Check if canceled during initialization
    if (tracked->canceled && !tracked->init_complete) {
        std::cout << theme::step(fmt::format("Job '{}' was canceled during initialization", arg));
        return;
    }

    // Check if init failed
    if (!tracked->init_error.empty()) {
        std::cout << theme::fail(fmt::format("Job '{}' init failed: {}", arg, tracked->init_error)) << "\n";
        return;
    }

    // Initializing: show init progress with incremental log tailing
    if (!tracked->init_complete) {
        enter_alt_screen();
        draw_header(arg, "INITIALIZING...");
        setup_content_area();

        std::string init_log = "/tmp/tccp_init_" + tracked->job_id + ".log";

        // Raw terminal for Ctrl+\ detection
        struct termios old_term, raw_term;
        tcgetattr(STDIN_FILENO, &old_term);
        raw_term = old_term;
        raw_term.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw_term.c_cc[VMIN] = 0;
        raw_term.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);

        std::streampos log_pos = 0;  // Track read position for incremental tailing

        while (true) {
            // Incremental tail: only read new content since last check
            std::ifstream f(init_log);
            if (f) {
                f.seekg(log_pos);
                std::string line;
                while (std::getline(f, line)) {
                    std::cout << theme::log(line) << std::flush;
                }
                if (f.eof()) {
                    f.clear();
                }
                log_pos = f.tellg();
            }

            // Poll for Ctrl+\ or Ctrl+C (500ms intervals)
            bool user_detached = false;
            bool user_canceled = false;
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    if (c == 0x1c) {
                        user_detached = true;
                    } else if (c == 0x03) {
                        user_canceled = true;
                    }
                }
            }

            if (user_detached) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                leave_alt_screen();
                std::cout << theme::step(fmt::format(
                    "Detached. Init continues in background. 'view {}' to check.", arg));
                return;
            }

            if (user_canceled) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                leave_alt_screen();
                std::cout << theme::step("Canceling job during initialization...");
                auto cancel_result = cli.jobs->cancel_job_by_id(tracked->job_id, nullptr);
                if (cancel_result.is_ok()) {
                    std::cout << theme::ok(fmt::format("Job '{}' canceled during initialization", arg));
                } else {
                    std::cout << theme::fail(cancel_result.error);
                }
                return;
            }

            // Refresh tracked job state
            tracked = cli.jobs->find_by_name(arg);
            if (!tracked) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                leave_alt_screen();
                return;
            }

            if (!tracked->init_error.empty()) {
                // Read any remaining log output
                std::ifstream f2(init_log);
                if (f2) {
                    f2.seekg(log_pos);
                    std::string line;
                    while (std::getline(f2, line)) {
                        std::cout << theme::log(line) << std::flush;
                    }
                }
                std::cout << "\n" << theme::fail(tracked->init_error) << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                leave_alt_screen();
                return;
            }

            if (tracked->init_complete) {
                // Flush remaining log
                std::ifstream f2(init_log);
                if (f2) {
                    f2.seekg(log_pos);
                    std::string line;
                    while (std::getline(f2, line)) {
                        std::cout << theme::log(line) << std::flush;
                    }
                }

                // Check if it was canceled during init
                if (tracked->canceled) {
                    std::cout << "\n" << theme::step("Job canceled during initialization") << std::flush;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                    leave_alt_screen();
                    std::remove(init_log.c_str());
                    return;
                }

                break;
            }
        }

        // Restore terminal before attaching (JobView sets its own raw mode)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);

        // Clean up init log and clear init output from screen
        std::remove(init_log.c_str());
        std::cout << "\033[H\033[J" << std::flush;

        // Transition to live job view
        draw_header(arg, "RUNNING on " + tracked->compute_node,
                    tracked->slurm_id, tracked->compute_node);
        std::cout.flush();

        std::string output_path = output_path_for(tracked->job_id);
        tracked->output_file = output_path;

        attach_to_job(cli, tracked, arg, false);
        return;
    }

    // Check if canceled during init (compute_node will be empty)
    if (tracked->canceled && tracked->compute_node.empty()) {
        std::cout << theme::step(fmt::format("Job '{}' was canceled during initialization", arg));
        return;
    }

    // Attach to job (live or completed) - always use JobView for consistent scrollback
    enter_alt_screen();

    std::string status;
    if (tracked->completed && tracked->canceled) {
        status = "CANCELED";
    } else if (tracked->completed) {
        status = tracked->exit_code == 0 ? "COMPLETED" : fmt::format("FAILED (exit {})", tracked->exit_code);
    } else {
        status = "RUNNING on " + tracked->compute_node;
    }

    draw_header(arg, status, tracked->slurm_id, tracked->compute_node);
    setup_content_area();
    std::cout.flush();

    attach_to_job(cli, tracked, arg, true);
}

static std::string format_duration(const std::string& start_time, const std::string& end_time = "") {
    if (start_time.empty()) return "-";

    // Parse ISO timestamp: YYYY-MM-DDTHH:MM:SS
    struct tm start_tm = {};
    if (strptime(start_time.c_str(), "%Y-%m-%dT%H:%M:%S", &start_tm) == nullptr) {
        return "?";
    }
    std::time_t start_t = mktime(&start_tm);

    std::time_t end_t;
    if (!end_time.empty()) {
        struct tm end_tm = {};
        if (strptime(end_time.c_str(), "%Y-%m-%dT%H:%M:%S", &end_tm) == nullptr) {
            return "?";
        }
        end_t = mktime(&end_tm);
    } else {
        end_t = std::time(nullptr);  // Still running
    }

    int seconds = static_cast<int>(std::difftime(end_t, start_t));
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (hours > 0) {
        return fmt::format("{}h{}m", hours, mins);
    } else if (mins > 0) {
        return fmt::format("{}m{}s", mins, secs);
    } else {
        return fmt::format("{}s", secs);
    }
}

static std::string format_timestamp(const std::string& iso_time) {
    if (iso_time.empty()) return "-";

    // Parse ISO timestamp: YYYY-MM-DDTHH:MM:SS
    struct tm tm_buf = {};
    if (strptime(iso_time.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_buf) == nullptr) {
        return "?";
    }

    // Format as "14:35" (24-hour time only)
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm_buf);
    return std::string(buf);
}

static void do_jobs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    // Query remote SLURM state first to ensure accurate status
    if (cli.allocs) {
        cli.allocs->reconcile(nullptr);
    }

    const auto& tracked = cli.jobs->tracked_jobs();
    if (tracked.empty()) {
        std::cout << theme::step("No tracked jobs.");
        return;
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<16} {:<20} {:<16} {:<8} {:<10} {:<14}",
                             "JOB", "STATUS", "TIMESTAMP", "WAIT", "DURATION", "NODE")
              << theme::color::RESET << "\n";

    for (const auto& tj : tracked) {
        std::string status;
        std::string timestamp = format_timestamp(tj.submit_time);
        std::string wait;
        std::string duration;

        if (!tj.init_error.empty()) {
            status = "INIT FAILED";
            wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);  // Still waiting
            duration = "-";
        } else if (tj.canceled && tj.compute_node.empty()) {
            status = "CANCELED (init)";
            wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);  // Time until canceled
            duration = "-";
        } else if (!tj.init_complete) {
            status = "INITIALIZING";
            wait = tj.submit_time.empty() ? "-" : format_duration(tj.submit_time);  // Currently waiting
            duration = "-";
        } else if (tj.completed && tj.canceled) {
            status = "CANCELED";
            wait = format_duration(tj.submit_time, tj.start_time);  // Init time
            duration = format_duration(tj.start_time, tj.end_time);
        } else if (tj.completed) {
            status = tj.exit_code == 0 ? "COMPLETED" : fmt::format("FAILED (exit {})", tj.exit_code);
            wait = format_duration(tj.submit_time, tj.start_time);  // Init time
            duration = format_duration(tj.start_time, tj.end_time);
        } else if (!tj.compute_node.empty()) {
            status = "RUNNING";
            wait = format_duration(tj.submit_time, tj.start_time);  // Init time
            duration = format_duration(tj.start_time);  // Still running
        } else {
            status = "PENDING";
            wait = "-";
            duration = "-";
        }

        std::cout << fmt::format("  {:<16} {:<20} {:<16} {:<8} {:<10} {:<14}\n",
                                  tj.job_name, status, timestamp, wait, duration, tj.compute_node);
    }
    std::cout << "\n";
}

static void do_cancel(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: cancel <job-name>");
        return;
    }

    auto cb = [](const std::string& msg) {
        std::cout << theme::step(msg);
    };

    auto result = cli.jobs->cancel_job(arg, cb);
    if (result.is_ok()) {
        std::cout << theme::ok(fmt::format("Canceled job '{}'", arg));
    } else {
        std::cout << theme::fail(result.error);
    }
}

static void do_return(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: return <job-id>");
        return;
    }

    auto cb = [](const std::string& msg) {
        std::cout << theme::ok(msg);
    };

    auto result = cli.jobs->return_output(arg, cb);
    if (result.is_err()) {
        std::cout << theme::fail(result.error);
    }
}

// ── Allocation commands ─────────────────────────────────────

static void do_allocs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.allocs) return;

    const auto& allocs = cli.allocs->allocations();
    if (allocs.empty()) {
        std::cout << theme::step("No active allocations.");
        return;
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<8} {:<14} {:<8} {}", "SLURM", "NODE", "STATUS", "TIME LEFT")
              << theme::color::RESET << "\n";

    for (const auto& a : allocs) {
        std::string status = a.active_job_id.empty() ? "IDLE" : "BUSY";
        // Approximate time remaining
        std::string time_left = "?";
        if (!a.start_time.empty()) {
            // Parse start time and compute remaining
            int total_min = a.duration_minutes;
            // Simple display: just show total duration for now
            int hours = total_min / 60;
            int mins = total_min % 60;
            time_left = fmt::format("{}h{}m alloc", hours, mins);
        }

        std::cout << fmt::format("  {:<8} {:<14} {:<8} {}\n",
                                  a.slurm_id, a.node, status, time_left);
    }
    std::cout << "\n";
}

static void do_dealloc(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.allocs) return;

    auto cb = [](const std::string& msg) {
        std::cout << theme::ok(msg);
    };

    if (arg.empty() || arg == "all") {
        cli.allocs->deallocate_all_idle(cb);
    } else {
        cli.allocs->deallocate(arg, cb);
    }
}

void register_jobs_commands(BaseCLI& cli) {
    cli.add_command("run", do_run, "Submit and attach to a job");
    cli.add_command("view", do_view, "Re-attach to a running job");
    cli.add_command("jobs", do_jobs, "List running jobs");
    cli.add_command("cancel", do_cancel, "Cancel a job");
    cli.add_command("return", do_return, "Download job output");
    cli.add_command("allocs", do_allocs, "List compute allocations");
    cli.add_command("dealloc", do_dealloc, "Release allocations");
}
