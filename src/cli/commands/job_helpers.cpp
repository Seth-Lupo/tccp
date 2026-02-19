#include "job_helpers.hpp"
#include "../job_view.hpp"
#include "../terminal_ui.hpp"
#include "../theme.hpp"
#include <core/utils.hpp>
#include <core/time_utils.hpp>
#include <platform/terminal.hpp>
#include <iostream>
#include <chrono>
#ifdef _WIN32
#  include <io.h>
#  define read  _read
#  define write _write
#  ifndef STDIN_FILENO
#    define STDIN_FILENO 0
#  endif
#  ifndef STDOUT_FILENO
#    define STDOUT_FILENO 1
#  endif
#else
#  include <unistd.h>
#endif
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
    platform::flush_stdin();
    platform::RawModeGuard raw_guard(platform::RawModeGuard::kNoEcho);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    bool first_esc = false;
    auto first_esc_time = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        if (platform::poll_stdin(static_cast<int>(std::min<long long>(remaining, 100)))) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 27) {
                    auto now = std::chrono::steady_clock::now();
                    if (first_esc && (now - first_esc_time) < std::chrono::milliseconds(500)) {
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

    return false;
}

bool attach_to_job(BaseCLI& cli, TrackedJob* tracked,
                   const std::string& job_name, bool replay,
                   bool in_alt_screen) {
    for (;;) {
        JobView view(*cli.service.cluster(),
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
        if (result.value == JobView::kViewOutput) {
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

        if (result.value == JobView::kCanceled) {
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
