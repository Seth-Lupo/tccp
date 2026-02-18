#include "job_helpers.hpp"
#include "../terminal_ui.hpp"
#include "../terminal_output.hpp"
#include "../theme.hpp"
#include <managers/job_log.hpp>
#include <core/constants.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <fmt/format.h>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

// Read new lines from init log since last position, print them, return updated position.
static std::streampos flush_init_log(const std::string& path, std::streampos pos) {
    std::ifstream f(path);
    if (!f) return pos;
    f.seekg(pos);
    std::string line;
    while (std::getline(f, line)) {
        std::cout << theme::log(line) << std::flush;
    }
    if (f.eof()) f.clear();
    return f.tellg();
}

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
            log_pos = flush_init_log(init_log, log_pos);

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
                flush_init_log(init_log, log_pos);
                std::cout << "\n" << theme::error(tracked->init_error) << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                TerminalUI::leave_alt_screen();
                return;
            }

            if (tracked->init_complete) {
                flush_init_log(init_log, log_pos);

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

        attach_to_job(cli, tracked, job_name, true);
        return;
    }

    // Check if canceled during init (compute_node will be empty)
    if (tracked->canceled && tracked->compute_node.empty()) {
        std::cout << fmt::format("Job '{}' was canceled during initialization", job_name) << "\n";
        return;
    }

    // Completed jobs: read log from NFS output dir (compute node may be inaccessible)
    if (tracked->completed) {
        auto* jm = cli.service.job_manager();
        std::string nfs_log = jm->job_output_dir(tracked->job_id) + "/tccp.log";
        std::string cat_cmd = fmt::format("cat {} 2>/dev/null", nfs_log);
        auto result = cli.service.cluster()->dtn().run(cat_cmd);
        if (result.stdout_data.empty()) {
            std::cout << theme::dim("No output available.") << "\n";
        } else {
            std::string clean = TerminalOutput::strip_ansi(
                result.stdout_data.data(),
                static_cast<int>(result.stdout_data.size()));
            if (!clean.empty())
                write(STDOUT_FILENO, clean.data(), clean.size());
            if (!clean.empty() && clean.back() != '\n')
                write(STDOUT_FILENO, "\n", 1);
        }
        return;
    }

    // Running job: attach via SSH relay
    TerminalUI::enter_alt_screen();

    std::string status = "RUNNING on " + tracked->compute_node;

    draw_job_header(job_name, status, tracked->slurm_id, tracked->compute_node);
    TerminalUI::setup_content_area();
    std::cout.flush();

    attach_to_job(cli, tracked, job_name, true);
}

void do_run(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    // Parse: "job_name [extra_args...]"
    std::string job_name_arg = arg;
    std::string extra_args;
    auto space = arg.find(' ');
    if (space != std::string::npos) {
        job_name_arg = arg.substr(0, space);
        extra_args = arg.substr(space + 1);
    }

    std::string job_name = resolve_job_name(cli, job_name_arg);
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

    auto result = cli.service.run_job(job_name, extra_args, nullptr);

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
    auto result = cli.service.run_job(job_name, "", nullptr);
    if (result.is_err()) {
        std::cout << theme::error(result.error);
        return;
    }

    do_view(cli, job_name);
}

void do_initlogs(BaseCLI& cli, const std::string& arg) {
    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    std::string log_path = job_log_path(tracked->job_id);
    std::ifstream f(log_path);
    if (!f) {
        std::cout << theme::dim("No init logs available.") << "\n";
        return;
    }

    std::string status;
    if (!tracked->init_error.empty())
        status = "init failed";
    else if (tracked->canceled)
        status = "canceled";
    else if (tracked->completed)
        status = tracked->exit_code == 0 ? "completed" : fmt::format("failed, exit {}", tracked->exit_code);
    else if (!tracked->init_complete)
        status = "initializing";
    else
        status = "running";

    std::cout << theme::dim(fmt::format("── {} initlogs ({}) ──", job_name, status)) << "\n";
    std::string line;
    while (std::getline(f, line)) {
        std::cout << line << "\n";
    }
    std::cout << theme::dim("── end ──") << "\n";
}

// Fetch job output: local returned copy > NFS > compute node.
// Returns raw output data (may contain ANSI). Empty string on failure (message already printed).
static std::string fetch_job_output(BaseCLI& cli, const TrackedJob* tracked) {
    auto* jm = cli.service.job_manager();

    // If output was already returned locally, read from disk
    if (tracked->output_returned) {
        namespace fs = std::filesystem;
        fs::path local_log = cli.service.config().project_dir()
            / "output" / JobManager::job_name_from_id(tracked->job_id)
            / JobManager::timestamp_from_id(tracked->job_id) / "tccp.log";
        if (fs::exists(local_log)) {
            std::ifstream f(local_log, std::ios::binary);
            if (f) {
                std::string data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
                return data;
            }
        }
    }

    // For completed jobs, try NFS output dir (may still exist if return hasn't run)
    if (tracked->completed) {
        std::string nfs_log = jm->job_output_dir(tracked->job_id) + "/tccp.log";
        auto result = cli.service.cluster()->dtn().run(
            fmt::format("cat {} 2>/dev/null", nfs_log));
        if (!result.stdout_data.empty())
            return result.stdout_data;

        std::cout << theme::dim("No output available.") << "\n";
        return "";
    }

    // Running job: read from compute node scratch
    if (tracked->compute_node.empty() || tracked->scratch_path.empty()) {
        std::cout << theme::dim("No output available.") << "\n";
        return "";
    }
    std::string remote_log = tracked->scratch_path + "/tccp.log";
    auto result = cli.service.cluster()->dtn().run(
        fmt::format("ssh {} {} 'cat {} 2>/dev/null'",
                     SSH_OPTS_FAST, tracked->compute_node, remote_log));
    if (result.stdout_data.empty()) {
        std::cout << theme::dim("No output available.") << "\n";
        return "";
    }
    return result.stdout_data;
}

void do_logs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    std::string raw = fetch_job_output(cli, tracked);
    if (raw.empty()) return;

    std::string status;
    if (tracked->completed && tracked->canceled)
        status = "canceled";
    else if (tracked->completed)
        status = tracked->exit_code == 0 ? "completed" : fmt::format("failed, exit {}", tracked->exit_code);
    else
        status = "running on " + tracked->compute_node;

    // Strip ANSI and noise (e.g. "Script started on ...")
    std::string clean = TerminalOutput::strip_ansi(raw.data(), static_cast<int>(raw.size()));
    TerminalOutput::suppress_noise(clean);

    std::cout << theme::dim(fmt::format("── {} logs ({}) ──", job_name, status)) << "\n";
    if (!clean.empty())
        write(STDOUT_FILENO, clean.data(), clean.size());
    if (!clean.empty() && clean.back() != '\n')
        write(STDOUT_FILENO, "\n", 1);
    std::cout << theme::dim("── end ──") << "\n";
}

void do_output(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    std::string raw = fetch_job_output(cli, tracked);
    if (raw.empty()) return;

    // Write to local temp file and open in vim
    std::string local_tmp = "/tmp/tccp_view_" + tracked->job_id + ".log";
    std::ofstream out(local_tmp, std::ios::binary);
    if (!out) {
        std::cout << theme::error("Failed to create temp file: " + local_tmp);
        return;
    }
    out.write(raw.data(), raw.size());
    out.close();

    std::string editor = cli.service.config().editor();
    std::string editor_cmd = editor + " " + local_tmp;
    std::system(editor_cmd.c_str());
    std::remove(local_tmp.c_str());
}

void do_tail(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    // Parse: "tail [job_name] [N]"
    // If arg is a pure number, treat as N with implicit job.
    // Otherwise first token is job name, optional second token is N.
    std::string job_name_arg;
    int num_lines = 30;

    if (!arg.empty()) {
        // Check if entire arg is a number
        bool is_number = true;
        for (char c : arg) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                is_number = false;
                break;
            }
        }

        if (is_number) {
            num_lines = std::stoi(arg);
        } else {
            auto space = arg.find(' ');
            if (space != std::string::npos) {
                job_name_arg = arg.substr(0, space);
                std::string rest = arg.substr(space + 1);
                bool rest_is_num = true;
                for (char c : rest) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        rest_is_num = false;
                        break;
                    }
                }
                if (rest_is_num && !rest.empty())
                    num_lines = std::stoi(rest);
            } else {
                job_name_arg = arg;
            }
        }
    }

    std::string job_name = resolve_job_name(cli, job_name_arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    std::string raw = fetch_job_output(cli, tracked);
    if (raw.empty()) return;

    std::string clean = TerminalOutput::strip_ansi(raw.data(), static_cast<int>(raw.size()));
    TerminalOutput::suppress_noise(clean);

    // Extract last N lines
    std::vector<std::string> lines;
    std::string::size_type pos = 0;
    while (pos < clean.size()) {
        auto nl = clean.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(clean.substr(pos));
            break;
        }
        lines.push_back(clean.substr(pos, nl - pos));
        pos = nl + 1;
    }

    std::string status;
    if (tracked->completed && tracked->canceled)
        status = "canceled";
    else if (tracked->completed)
        status = tracked->exit_code == 0 ? "completed" : fmt::format("failed, exit {}", tracked->exit_code);
    else
        status = "running on " + tracked->compute_node;

    std::cout << theme::dim(fmt::format("── {} tail (last {}, {}) ──",
                                         job_name, num_lines, status)) << "\n";

    int start = static_cast<int>(lines.size()) - num_lines;
    if (start < 0) start = 0;
    for (int i = start; i < static_cast<int>(lines.size()); ++i) {
        std::cout << lines[i] << "\n";
    }

    std::cout << theme::dim("── end ──") << "\n";
}
