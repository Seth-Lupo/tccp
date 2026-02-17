#include "job_helpers.hpp"
#include "../job_view.hpp"
#include "../terminal_ui.hpp"
#include "../theme.hpp"
#include <core/utils.hpp>
#include <core/time_utils.hpp>
#include <iostream>
#include <chrono>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <fmt/format.h>

std::string dtach_sock_for(const TrackedJob& tj) {
    return tj.scratch_path + "/tccp.sock";
}

void draw_job_header(const std::string& job_name,
                     const std::string& status,
                     const std::string& slurm_id,
                     const std::string& node) {
    TerminalUI::StatusBar bar;
    bar.job_name = job_name;
    bar.slurm_id = slurm_id;
    bar.node = node;
    bar.status = status;
    bar.controls = (status == "INITIALIZING...")
        ? "Ctrl+C cancel  Ctrl+\\ detach "
        : "Ctrl+\\ to detach ";
    TerminalUI::draw_status_bar(bar);
}

bool wait_or_detach(int ms) {
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

bool attach_to_job(BaseCLI& cli, TrackedJob* tracked,
                   const std::string& job_name, bool replay,
                   bool in_alt_screen) {
    for (;;) {
        JobView view(*cli.service.cluster()->get_session(),
                     tracked->compute_node,
                     get_cluster_username(),
                     dtach_sock_for(*tracked),
                     job_name,
                     tracked->slurm_id,
                     tracked->job_id,
                     cli.service.config().dtn().host,
                     tracked->scratch_path,
                     tracked->canceled);
        auto result = view.attach(replay);

        if (result.is_err()) {
            if (in_alt_screen) TerminalUI::leave_alt_screen();
            std::cout << theme::error("Attach failed: " + result.error);
            return false;
        }

        // Ctrl+V: view output in vim, then reattach
        if (result.value == -3) {
            if (in_alt_screen) TerminalUI::leave_alt_screen();
            do_output(cli, job_name);
            // Re-enter alt screen and reattach
            if (in_alt_screen) {
                TerminalUI::enter_alt_screen();
                std::string status = "RUNNING on " + tracked->compute_node;
                draw_job_header(job_name, status, tracked->slurm_id, tracked->compute_node);
                TerminalUI::setup_content_area();
                std::cout.flush();
            }
            replay = true;  // skip remote replay on reattach
            continue;
        }

        if (in_alt_screen) TerminalUI::leave_alt_screen();

        if (result.value == -2) {
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
}

std::string resolve_job_name(BaseCLI& cli, const std::string& arg) {
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
