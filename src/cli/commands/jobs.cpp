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

static void replay_captured_output(const std::string& path) {
    std::ifstream f(path);
    if (f.is_open()) {
        std::cout << f.rdbuf() << std::flush;
    }
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
    std::string l2_right = "Ctrl+\\\\ to detach ";
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

    if (replay) {
        replay_captured_output(output_path);
    }

    JobView view(*cli.cluster->get_session(),
                 tracked->compute_node,
                 get_username(),
                 dtach_sock_for(tracked->job_id),
                 job_name,
                 tracked->slurm_id,
                 output_path);
    auto result = view.attach(replay);
    if (in_alt_screen) leave_alt_screen();

    if (result.is_err()) {
        std::cout << theme::fail("Attach failed: " + result.error);
        return false;
    }

    if (result.value >= 0) {
        tracked->exit_code = result.value;
        tracked->completed = true;
        if (cli.jobs) cli.jobs->on_job_complete(*tracked);
        std::cout << theme::dim("    Session ended.") << "\n";
        return true;
    }

    std::cout << theme::dim("    Detached. Use 'view " + job_name + "' to re-attach.") << "\n";
    return false;
}

// ── Commands ────────────────────────────────────────────────

static void do_run(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: run <job-name>");
        return;
    }

    auto* existing = cli.jobs->find_by_name(arg);
    if (existing && !existing->completed) {
        std::cout << theme::fail(fmt::format("Job '{}' is already running (SLURM:{}). "
            "Cancel it first or wait for it to finish.", arg, existing->slurm_id));
        return;
    }

    // Enter alt screen immediately — everything happens here
    enter_alt_screen();
    draw_header(arg, "Initializing...");
    setup_content_area();

    // Capture file for init logs + job output (keyed on job name temporarily)
    std::string output_path = output_path_for(arg);
    FILE* init_capture = fopen(output_path.c_str(), "w");

    // Raw terminal so Ctrl+\ works during init
    struct termios old_term, raw_term;
    tcgetattr(STDIN_FILENO, &old_term);
    raw_term = old_term;
    raw_term.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw_term.c_cc[VMIN] = 0;
    raw_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);

    bool detached = false;
    auto cb = [&init_capture, &detached](const std::string& msg) {
        std::cout << theme::log(msg) << std::flush;
        if (init_capture) {
            std::string plain = msg + "\n";
            fwrite(plain.data(), 1, plain.size(), init_capture);
            fflush(init_capture);
        }
        // Check for Ctrl+backslash
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && c == 0x1c)
                detached = true;
        }
    };

    auto result = cli.jobs->run(arg, cb);

    // Restore terminal
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);

    if (init_capture) { fclose(init_capture); init_capture = nullptr; }

    if (detached) {
        leave_alt_screen();
        auto* tracked = cli.jobs->find_by_name(arg);
        if (tracked) {
            std::string real_output = output_path_for(tracked->job_id);
            std::rename(output_path.c_str(), real_output.c_str());
            tracked->output_file = real_output;
        }
        std::cout << theme::dim(fmt::format("    Detached. Use 'view {}' to re-attach.", arg)) << "\n";
        return;
    }

    if (result.is_err()) {
        draw_header(arg, "Failed");
        std::cout << theme::fail(result.error);
        std::cout << "\n" << theme::dim("    Press Enter to return...") << std::flush;
        std::string dummy;
        std::getline(std::cin, dummy);
        leave_alt_screen();
        std::remove(output_path.c_str());
        return;
    }

    auto* tracked = cli.jobs->find_by_name(arg);
    if (!tracked) { leave_alt_screen(); return; }

    // Rename capture to real job_id path
    std::string real_output = output_path_for(tracked->job_id);
    if (output_path != real_output) {
        std::rename(output_path.c_str(), real_output.c_str());
    }
    tracked->output_file = real_output;

    // Update header, then attach — NO clearing, job output continues after init logs
    draw_header(arg, "RUNNING on " + tracked->compute_node,
                tracked->slurm_id, tracked->compute_node);

    attach_to_job(cli, tracked, arg, false);
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

    // Completed job: show captured output read-only
    if (tracked->completed) {
        enter_alt_screen();
        std::string status = tracked->exit_code == 0 ? "COMPLETED" :
            fmt::format("FAILED (exit {})", tracked->exit_code);
        draw_header(arg, status, tracked->slurm_id, tracked->compute_node);
        setup_content_area();

        replay_captured_output(tracked->output_file);

        // Position exit prompt on the last line of the scroll region
        int rows = term_rows();
        int prompt_row = std::max(1, rows - 2);
        std::cout << fmt::format("\033[{};1H", prompt_row);
        if (tracked->exit_code == 0) {
            std::cout << theme::dim("[exit 0]") << "  ";
        } else {
            std::cout << theme::red(fmt::format("[exit {}]", tracked->exit_code)) << "  ";
        }
        std::cout << theme::dim("Press Enter to leave...") << std::flush;
        std::string dummy;
        std::getline(std::cin, dummy);
        leave_alt_screen();
        return;
    }

    // Running job: attach live
    enter_alt_screen();
    setup_content_area();

    draw_header(arg, "RUNNING on " + tracked->compute_node + " | SLURM:" + tracked->slurm_id,
                tracked->slurm_id, tracked->compute_node);

    attach_to_job(cli, tracked, arg, true);
}

static void do_jobs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    const auto& tracked = cli.jobs->tracked_jobs();
    if (tracked.empty()) {
        std::cout << theme::dim("  No tracked jobs.") << "\n";
        return;
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<8} {:<16} {:<14} {}", "SLURM", "JOB", "STATUS", "NODE")
              << theme::color::RESET << "\n";

    for (const auto& tj : tracked) {
        std::string status;
        if (tj.completed)
            status = tj.exit_code == 0 ? "COMPLETED" : fmt::format("FAILED({})", tj.exit_code);
        else if (!tj.compute_node.empty())
            status = "RUNNING";
        else
            status = "PENDING";

        std::cout << fmt::format("  {:<8} {:<16} {:<14} {}\n",
                                  tj.slurm_id, tj.job_name, status, tj.compute_node);
    }
    std::cout << "\n";
}

static void do_cancel(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.jobs) return;

    if (arg.empty()) {
        std::cout << theme::fail("Usage: cancel <slurm-id>");
        return;
    }

    auto result = cli.jobs->cancel(arg);
    if (result.success()) {
        std::cout << theme::ok(fmt::format("Cancelled job {}", arg));
    } else {
        std::cout << theme::fail(fmt::format("Failed to cancel: {}", result.get_output()));
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
        std::cout << theme::dim("  No active allocations.") << "\n";
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
