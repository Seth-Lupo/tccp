#include "job_helpers.hpp"
#include "../terminal_ui.hpp"
#include "../theme.hpp"
#include <managers/job_log.hpp>
#include <core/constants.hpp>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <fmt/format.h>
#include <fstream>

void do_view(BaseCLI& cli, const std::string& arg) {
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
        TerminalUI::enter_alt_screen();
        draw_job_header(job_name, "INITIALIZING...");
        TerminalUI::setup_content_area();

        std::string init_log = job_log_path(tracked->job_id);

        // Raw terminal for Ctrl+\ detection
        struct termios old_term, raw_term;
        tcgetattr(STDIN_FILENO, &old_term);
        raw_term = old_term;
        raw_term.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw_term.c_cc[VMIN] = 0;
        raw_term.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_term);

        std::streampos log_pos = 0;

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
                TerminalUI::leave_alt_screen();
                std::cout << fmt::format(
                    "Detached. Init continues in background. 'view {}' to check.", job_name) << "\n";
                return;
            }

            if (user_canceled) {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                TerminalUI::leave_alt_screen();
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
                TerminalUI::leave_alt_screen();
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
                TerminalUI::leave_alt_screen();
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
                    TerminalUI::leave_alt_screen();
                    return;
                }

                break;
            }
        }

        // Restore terminal before attaching (JobView sets its own raw mode)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);

        // Clear init output from screen
        std::cout << "\033[H\033[J" << std::flush;

        // Transition to live job view
        draw_job_header(job_name, "RUNNING on " + tracked->compute_node,
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
    TerminalUI::enter_alt_screen();

    std::string status;
    if (tracked->completed && tracked->canceled) {
        status = "CANCELED";
    } else if (tracked->completed) {
        status = tracked->exit_code == 0 ? "COMPLETED" : fmt::format("FAILED (exit {})", tracked->exit_code);
    } else {
        status = "RUNNING on " + tracked->compute_node;
    }

    draw_job_header(job_name, status, tracked->slurm_id, tracked->compute_node);
    TerminalUI::setup_content_area();
    std::cout.flush();

    attach_to_job(cli, tracked, job_name, true);
}

void do_run(BaseCLI& cli, const std::string& arg) {
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

    auto result = cli.service.run_job(job_name, nullptr);

    if (result.is_err()) {
        std::cout << theme::error(result.error);
        return;
    }

    // Immediately attach to see init progress (user can Ctrl+\ to detach)
    do_view(cli, job_name);
}

void do_restart(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* existing = cli.service.find_job_by_name(job_name);
    if (existing && !existing->completed && existing->init_error.empty()) {
        // Job is active — cancel it first
        std::cout << theme::dim(fmt::format("Canceling '{}'...", job_name)) << "\n";
        auto cancel_result = cli.service.cancel_job(job_name, nullptr);
        if (cancel_result.is_err()) {
            std::cout << theme::error(cancel_result.error);
            return;
        }

        // Wait briefly for cancellation to settle
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Now submit fresh
    auto result = cli.service.run_job(job_name, nullptr);
    if (result.is_err()) {
        std::cout << theme::error(result.error);
        return;
    }

    do_view(cli, job_name);
}

void do_tail(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    // Parse args: "job_name" or "job_name N" for number of lines
    std::string job_name_arg = arg;
    int num_lines = 30;
    auto space = arg.rfind(' ');
    if (space != std::string::npos) {
        std::string maybe_num = arg.substr(space + 1);
        try {
            int n = std::stoi(maybe_num);
            if (n > 0) {
                num_lines = n;
                job_name_arg = arg.substr(0, space);
            }
        } catch (...) {}
    }

    std::string job_name = resolve_job_name(cli, job_name_arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    // For initializing jobs, tail the local init log
    if (!tracked->init_complete) {
        if (!tracked->init_error.empty()) {
            std::cout << theme::error(fmt::format("Job '{}' init failed: {}", job_name, tracked->init_error)) << "\n";
            return;
        }
        std::string init_log = job_log_path(tracked->job_id);
        std::ifstream f(init_log);
        if (!f) {
            std::cout << theme::dim("Initializing... no log output yet.") << "\n";
            return;
        }
        // Read all lines, keep last N
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
        }
        int start = std::max(0, static_cast<int>(lines.size()) - num_lines);
        std::cout << theme::dim(fmt::format("── {} (initializing) ──", job_name)) << "\n";
        for (int i = start; i < static_cast<int>(lines.size()); i++) {
            std::cout << theme::log(lines[i]) << std::flush;
        }
        return;
    }

    // For running/completed jobs, tail the remote log on compute node
    if (tracked->compute_node.empty()) {
        std::cout << theme::error("No compute node assigned");
        return;
    }

    std::string remote_log = dtach_sock_for(tracked->job_id);
    auto dot = remote_log.rfind('.');
    if (dot != std::string::npos)
        remote_log = remote_log.substr(0, dot) + ".log";

    std::string ssh_cmd = fmt::format(
        "ssh {} {} 'tail -n {} {} 2>/dev/null'",
        SSH_OPTS_FAST, tracked->compute_node, num_lines, remote_log);

    auto result = cli.service.cluster()->dtn().run(ssh_cmd);

    if (result.stdout_data.empty()) {
        std::cout << theme::dim("No output yet.") << "\n";
        return;
    }

    std::string status_label;
    if (tracked->completed && tracked->canceled) {
        status_label = "canceled";
    } else if (tracked->completed) {
        status_label = tracked->exit_code == 0 ? "completed" : fmt::format("failed, exit {}", tracked->exit_code);
    } else {
        status_label = "running on " + tracked->compute_node;
    }

    std::cout << theme::dim(fmt::format("── {} ({}) ──", job_name, status_label)) << "\n";
    std::cout << result.stdout_data;
    if (!result.stdout_data.empty() && result.stdout_data.back() != '\n') {
        std::cout << "\n";
    }
}
