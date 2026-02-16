#include "job_helpers.hpp"
#include "../theme.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <iostream>
#include <cstdlib>
#include <fmt/format.h>

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
              << fmt::format("  {:<16} {:<30} {:<16} {:<8} {:<10} {:<14}",
                             "JOB", "STATUS", "TIMESTAMP", "WAIT", "DURATION", "NODE")
              << theme::color::RESET << "\n";

    for (const auto& s : summaries) {
        std::string status = s.status;
        if (!s.ports.empty() && status == "RUNNING") {
            status = fmt::format("RUNNING (ports: {})", s.ports);
        }
        std::cout << fmt::format("  {:<16} {:<30} {:<16} {:<8} {:<10} {:<14}\n",
                                  s.job_name, status, s.timestamp, s.wait, s.duration, s.compute_node);
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
        std::cout << theme::dim(fmt::format("Canceled job '{}'", job_name)) << "\n";
    } else {
        std::cout << theme::error(result.error);
    }
}

static void do_return(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

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

void do_ssh(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    // Parse: "job_name [command...]"
    std::string job_name_arg = arg;
    std::string remote_cmd;
    auto space = arg.find(' ');
    if (space != std::string::npos) {
        job_name_arg = arg.substr(0, space);
        remote_cmd = arg.substr(space + 1);
    }

    std::string job_name = resolve_job_name(cli, job_name_arg);
    if (job_name.empty()) return;

    auto* tracked = cli.service.find_job_by_name(job_name);
    if (!tracked) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    if (tracked->compute_node.empty()) {
        if (!tracked->init_complete) {
            std::cout << theme::error("Job is still initializing (no compute node yet)");
        } else {
            std::cout << theme::error("No compute node assigned to this job");
        }
        return;
    }

    // Build the nested SSH command: local -> DTN -> compute node
    std::string dtn_host = cli.service.config().dtn().host;
    std::string user = get_cluster_username();

    if (remote_cmd.empty()) {
        // Interactive: use system ssh so the user gets a real terminal
        // Land in the job's scratch directory if available
        std::string inner_cmd = tracked->scratch_path.empty()
            ? fmt::format("ssh {} {}", SSH_OPTS, tracked->compute_node)
            : fmt::format("ssh {} {} \"cd {} && exec bash\"",
                          SSH_OPTS, tracked->compute_node, tracked->scratch_path);
        std::string sys_cmd = fmt::format(
            "ssh -t {} {}@{} '{}'",
            SSH_OPTS, user, dtn_host, inner_cmd);
        std::system(sys_cmd.c_str());
    } else {
        // One-off command: use libssh2 through DTN
        std::string ssh_cmd = fmt::format(
            "ssh {} {} '{}'",
            SSH_OPTS, tracked->compute_node, remote_cmd);

        auto result = cli.service.cluster()->dtn().run(ssh_cmd);
        std::cout << result.stdout_data;
        if (!result.stderr_data.empty()) {
            std::cerr << result.stderr_data;
        }
    }
}

void register_jobs_commands(BaseCLI& cli) {
    cli.add_command("run", do_run, "Submit and attach to a job");
    cli.add_command("view", do_view, "Re-attach to a running job");
    cli.add_command("restart", do_restart, "Cancel and re-run a job");
    cli.add_command("tail", do_tail, "Show last N lines of job output");
    cli.add_command("ssh", do_ssh, "Run command on job's compute node");
    cli.add_command("jobs", do_jobs, "List running jobs");
    cli.add_command("cancel", do_cancel, "Cancel a job");
    cli.add_command("return", do_return, "Download job output");
    cli.add_command("logs", do_logs, "View job lifecycle logs");
    cli.add_command("clean", do_clean, "Remove old output (keeps latest)");
}
