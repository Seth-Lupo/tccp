#include "../base_cli.hpp"
#include "../job_view.hpp"
#include "../theme.hpp"
#include <core/utils.hpp>
#include <core/time_utils.hpp>
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
#include <ctime>

// ── Helpers ──────────────────────────────────────────────────

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

    JobView view(*cli.service.cluster()->get_session(),
                 tracked->compute_node,
                 get_cluster_username(),
                 dtach_sock_for(tracked->job_id),
                 job_name,
                 tracked->slurm_id,
                 output_path,
                 tracked->job_id,
                 tracked->canceled);
    auto result = view.attach(replay);
    if (in_alt_screen) leave_alt_screen();

    if (result.is_err()) {
        std::cout << theme::error("Attach failed: " + result.error);
        return false;
    }

    if (result.value == -2) {
        // User pressed Ctrl+C to cancel job
        std::cout << "Canceling job...\n";
        auto cancel_result = cli.service.cancel_job_by_id(tracked->job_id, nullptr);
        if (cancel_result.is_ok()) {
            std::cout << theme::dim(fmt::format("Job '{}' canceled", job_name)) << "\n";
        } else {
            std::cout << theme::error(cancel_result.error);
        }
        return false;
    }

    if (result.value >= 0) {
        tracked->exit_code = result.value;
        tracked->completed = true;
        if (cli.service.job_manager()) cli.service.job_manager()->on_job_complete(*tracked);
        std::cout << "Session ended.\n";
        return true;
    }

    std::cout << "Detached. Use 'view " + job_name + "' to re-attach.\n";
    return false;
}

// ── Default job resolution ───────────────────────────────────

// Resolve a default job name when the user omits it.
// Returns the job name, or empty string on failure (after printing an error).
static std::string resolve_job_name(BaseCLI& cli, const std::string& arg) {
    if (!arg.empty()) return arg;

    if (!cli.service.has_config()) {
        std::cout << theme::error("No config loaded");
        return "";
    }

    const auto& jobs = cli.service.config().project().jobs;
    if (jobs.size() == 1) {
        return jobs.begin()->first;
    }
    if (jobs.empty()) {
        std::cout << theme::error("No jobs configured in tccp.yml");
        return "";
    }
    // Multiple jobs — try "main" as default
    if (jobs.count("main")) {
        return "main";
    }
    std::string names;
    for (const auto& [name, _] : jobs) {
        if (!names.empty()) names += ", ";
        names += name;
    }
    std::cout << theme::error(fmt::format(
        "Multiple jobs configured ({}). Specify which one.", names));
    return "";
}

// ── Commands ────────────────────────────────────────────────

static void do_view(BaseCLI& cli, const std::string& arg);  // forward decl

static void do_run(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* existing = cli.service.find_job_by_name(job_name);
    if (existing && !existing->completed && existing->init_error.empty()) {
        if (!existing->init_complete) {
            std::cout << theme::error(fmt::format("Job '{}' is still initializing. "
                "Use 'view {}' to check progress.", job_name, job_name));
        } else {
            std::cout << theme::error(fmt::format("Job '{}' is already running. "
                "Cancel it first or wait for it to finish.", job_name));
        }
        return;
    }

    // Start the job - this spawns background init and returns immediately
    auto result = cli.service.run_job(job_name, nullptr);

    if (result.is_err()) {
        std::cout << theme::error(result.error);
        return;
    }

    // Immediately attach to see init progress (user can Ctrl+\ to detach)
    do_view(cli, job_name);
}

static void do_view(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    // Check if canceled during initialization
    if (tracked->canceled && !tracked->init_complete) {
        std::cout << fmt::format("Job '{}' was canceled during initialization", job_name) << "\n";
        return;
    }

    // Check if init failed
    if (!tracked->init_error.empty()) {
        std::cout << theme::error(fmt::format("Job '{}' init failed: {}", job_name, tracked->init_error)) << "\n";
        return;
    }

    // Initializing: show init progress with incremental log tailing
    if (!tracked->init_complete) {
        enter_alt_screen();
        draw_header(job_name, "INITIALIZING...");
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
                std::cout << fmt::format(
                    "Detached. Init continues in background. 'view {}' to check.", job_name) << "\n";
                return;
            }

            if (user_canceled) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                leave_alt_screen();
                std::cout << "Canceling job during initialization...\n";
                auto cancel_result = cli.service.cancel_job_by_id(tracked->job_id, nullptr);
                if (cancel_result.is_ok()) {
                    std::cout << theme::dim(fmt::format("Job '{}' canceled during initialization", job_name)) << "\n";
                } else {
                    std::cout << theme::error(cancel_result.error);
                }
                return;
            }

            // Refresh tracked job state
            tracked = cli.service.find_job_by_name(job_name);
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
                std::cout << "\n" << theme::error(tracked->init_error) << std::flush;
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
                    std::cout << "\nJob canceled during initialization\n" << std::flush;
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
        draw_header(job_name, "RUNNING on " + tracked->compute_node,
                    tracked->slurm_id, tracked->compute_node);
        std::cout.flush();

        std::string output_path = output_path_for(tracked->job_id);
        tracked->output_file = output_path;

        attach_to_job(cli, tracked, job_name, false);
        return;
    }

    // Check if canceled during init (compute_node will be empty)
    if (tracked->canceled && tracked->compute_node.empty()) {
        std::cout << fmt::format("Job '{}' was canceled during initialization", job_name) << "\n";
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

    draw_header(job_name, status, tracked->slurm_id, tracked->compute_node);
    setup_content_area();
    std::cout.flush();

    attach_to_job(cli, tracked, job_name, true);
}

static void do_jobs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    auto summaries = cli.service.list_jobs();
    if (summaries.empty()) {
        std::cout << "No tracked jobs.\n";
        return;
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<16} {:<20} {:<16} {:<8} {:<10} {:<14}",
                             "JOB", "STATUS", "TIMESTAMP", "WAIT", "DURATION", "NODE")
              << theme::color::RESET << "\n";

    for (const auto& s : summaries) {
        std::cout << fmt::format("  {:<16} {:<20} {:<16} {:<8} {:<10} {:<14}\n",
                                  s.job_name, s.status, s.timestamp, s.wait, s.duration, s.compute_node);
    }
    std::cout << "\n";
}

static void do_cancel(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto cb = [](const std::string& msg) {
        std::cout << msg << "\n";
    };

    auto result = cli.service.cancel_job(job_name, cb);
    if (result.is_ok()) {
        std::cout << theme::dim(fmt::format("Canceled job '{}'", job_name));
    } else {
        std::cout << theme::error(result.error);
    }
}

static void do_return(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    // Resolve job name → job ID
    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    auto cb = [](const std::string& msg) {
        std::cout << theme::dim(msg);
    };

    auto result = cli.service.return_output(tracked->job_id, cb);
    if (result.is_err()) {
        std::cout << theme::error(result.error);
    }
}

static void do_clean(BaseCLI& cli, const std::string& arg) {
    namespace fs = std::filesystem;

    fs::path output_root = fs::current_path() / "output";
    if (!fs::exists(output_root) || !fs::is_directory(output_root)) {
        std::cout << "No output directory found.\n";
        return;
    }

    int removed = 0;
    std::error_code ec;

    for (const auto& job_entry : fs::directory_iterator(output_root)) {
        if (!job_entry.is_directory()) continue;
        std::string job_name = job_entry.path().filename().string();

        // If a specific job name was given, only clean that one
        if (!arg.empty() && job_name != arg) continue;

        // Find what "latest" points to so we can keep it
        fs::path latest_link = job_entry.path() / "latest";
        std::string keep_name;
        if (fs::is_symlink(latest_link)) {
            keep_name = fs::read_symlink(latest_link, ec).string();
        }

        for (const auto& run_entry : fs::directory_iterator(job_entry.path())) {
            std::string name = run_entry.path().filename().string();
            if (name == "latest") continue;           // don't delete the symlink
            if (name == keep_name) continue;           // don't delete what latest points to
            fs::remove_all(run_entry.path(), ec);
            removed++;
        }
    }

    if (removed > 0) {
        std::cout << fmt::format("Cleaned {} old output run(s). Latest kept.\n", removed);
    } else {
        std::cout << "Nothing to clean.\n";
    }
}

void register_jobs_commands(BaseCLI& cli) {
    cli.add_command("run", do_run, "Submit and attach to a job");
    cli.add_command("view", do_view, "Re-attach to a running job");
    cli.add_command("jobs", do_jobs, "List running jobs");
    cli.add_command("cancel", do_cancel, "Cancel a job");
    cli.add_command("return", do_return, "Download job output");
    cli.add_command("clean", do_clean, "Remove old output (keeps latest)");
}
