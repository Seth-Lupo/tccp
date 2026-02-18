#include "job_helpers.hpp"
#include "../theme.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <core/time_utils.hpp>
#include <managers/job_log.hpp>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <fmt/format.h>

static void do_jobs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    auto summaries = cli.service.list_jobs();
    if (summaries.empty()) {
        std::cout << "No tracked jobs.\n";
        return;
    }

    // Build display data and compute column widths
    struct Row { std::string job, status, time, wait, dur, node; };
    std::vector<Row> rows;
    rows.reserve(summaries.size());
    for (const auto& s : summaries) {
        Row r;
        r.job = s.job_name;
        r.status = s.status;
        if (!s.ports.empty() && r.status == "RUNNING")
            r.status = fmt::format("RUNNING (ports: {})", s.ports);
        r.time = s.timestamp;
        r.wait = s.wait;
        r.dur = s.duration;
        r.node = s.compute_node;
        rows.push_back(std::move(r));
    }

    size_t w0 = 3, w1 = 6, w2 = 4, w3 = 4, w4 = 8, w5 = 4; // header lengths
    for (const auto& r : rows) {
        w0 = std::max(w0, r.job.size());
        w1 = std::max(w1, r.status.size());
        w2 = std::max(w2, r.time.size());
        w3 = std::max(w3, r.wait.size());
        w4 = std::max(w4, r.dur.size());
        w5 = std::max(w5, r.node.size());
    }

    std::string hfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    w0 + 2, w1 + 2, w2 + 2, w3 + 2, w4 + 2);
    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(hfmt), "JOB", "STATUS", "TIME", "WAIT", "DURATION", "NODE")
              << theme::color::RESET;
    for (const auto& r : rows) {
        std::cout << fmt::format(fmt::runtime(hfmt), r.job, r.status, r.time, r.wait, r.dur, r.node);
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

    std::string dtn_host = cli.service.config().dtn().host;
    std::string user = get_cluster_username();

    // Special case: "ssh login" — interactive shell on login node via DTN
    if (arg == "login" || arg.empty()) {
        std::string login_host = cli.service.config().login().host;
        // Hop: local -> DTN -> login node (uses DTN's internal auth)
        std::string sys_cmd = fmt::format(
            "ssh -t -o StrictHostKeyChecking=no {}@{} "
            "'ssh -t -o StrictHostKeyChecking=no {}'",
            user, dtn_host, login_host);
        std::system(sys_cmd.c_str());
        return;
    }

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

    // Build the environment-wrapped command
    auto* jm = cli.service.job_manager();
    std::string shell_cmd = jm->shell_command(*tracked, remote_cmd);

    if (remote_cmd.empty()) {
        // Interactive: local -> DTN -> compute node
        std::string sys_cmd = fmt::format(
            "ssh -t -o StrictHostKeyChecking=no {}@{} "
            "'ssh -t -o StrictHostKeyChecking=no {} '\\''{}'\\'",
            user, dtn_host,
            tracked->compute_node, shell_cmd);
        std::system(sys_cmd.c_str());
    } else {
        // One-off command: use libssh2 through DTN
        std::string ssh_cmd = fmt::format(
            "ssh {} {} '{}'",
            SSH_OPTS, tracked->compute_node, shell_cmd);

        auto result = cli.service.cluster()->dtn().run(ssh_cmd);
        std::cout << result.stdout_data;
        if (!result.stderr_data.empty()) {
            std::cerr << result.stderr_data;
        }
    }
}

void do_open(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    const std::string job_name = "bash";

    // Cancel any existing bash job
    auto* existing = cli.service.find_job_by_name(job_name);
    if (existing && !existing->completed && existing->init_error.empty()) {
        std::cout << theme::dim("Cleaning up previous session...") << "\n";
        cli.service.cancel_job(job_name, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Start a fresh bash job
    auto result = cli.service.run_job(job_name, "", nullptr);
    if (result.is_err()) {
        std::cout << theme::error(result.error);
        return;
    }

    // Wait for init to complete, showing progress
    std::cout << theme::dim("Setting up remote environment...") << "\n";

    std::string log_path = job_log_path(
        cli.service.find_job_by_name(job_name)->job_id);
    std::streampos log_pos = 0;

    while (true) {
        // Print incremental init log
        std::ifstream f(log_path);
        if (f) {
            f.seekg(log_pos);
            std::string line;
            while (std::getline(f, line)) {
                std::cout << theme::dim("  " + line) << "\n";
            }
            if (f.eof()) f.clear();
            log_pos = f.tellg();
        }

        auto* tj = cli.service.find_job_by_name(job_name);
        if (!tj) {
            std::cout << theme::error("Job disappeared during initialization");
            return;
        }

        if (!tj->init_error.empty()) {
            std::cout << theme::error(tj->init_error);
            return;
        }

        if (tj->init_complete) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << theme::dim("Connected.") << "\n\n";

    // SSH into the bash job (reuse do_ssh logic)
    do_ssh(cli, job_name);

    // Clean up after SSH exits
    std::cout << "\n" << theme::dim("Closing session...") << "\n";
    cli.service.cancel_job(job_name, nullptr);
}

void do_config(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_config()) return;

    const auto& proj = cli.service.config().project();

    std::cout << "\n";
    std::cout << theme::kv("Project", proj.name);
    std::cout << theme::kv("Type", proj.type);

    if (!proj.output.empty())
        std::cout << theme::kv("Output", proj.output);
    if (!proj.cache.empty())
        std::cout << theme::kv("Cache", proj.cache);
    if (!proj.env.empty())
        std::cout << theme::kv("Env", proj.env);
    if (!proj.rodata.empty()) {
        std::string rodata_str;
        for (const auto& r : proj.rodata) {
            if (!rodata_str.empty()) rodata_str += ", ";
            rodata_str += r;
        }
        std::cout << theme::kv("Rodata", rodata_str);
    }

    for (const auto& [name, job] : proj.jobs) {
        std::cout << "\n" << theme::dim(fmt::format("    Job: {}", name)) << "\n";
        if (!job.script.empty())
            std::cout << theme::kv("Script", job.script);
        if (!job.package.empty())
            std::cout << theme::kv("Package", job.package);
        if (!job.args.empty())
            std::cout << theme::kv("Args", job.args);
        if (!job.time.empty())
            std::cout << theme::kv("Time", job.time);
        if (!job.ports.empty()) {
            std::string ports_str;
            for (int p : job.ports) {
                if (!ports_str.empty()) ports_str += ", ";
                ports_str += std::to_string(p);
            }
            std::cout << theme::kv("Ports", ports_str);
        }
        if (job.slurm) {
            const auto& s = *job.slurm;
            if (!s.partition.empty())
                std::cout << theme::kv("Partition", s.partition);
            if (!s.gpu_type.empty())
                std::cout << theme::kv("GPU", fmt::format("{}x{}", s.gpu_count, s.gpu_type));
        }
    }

    if (proj.slurm) {
        const auto& s = *proj.slurm;
        std::cout << "\n" << theme::dim("    SLURM defaults") << "\n";
        if (!s.partition.empty())
            std::cout << theme::kv("Partition", s.partition);
        if (!s.gpu_type.empty())
            std::cout << theme::kv("GPU", fmt::format("{}x{}", s.gpu_count, s.gpu_type));
        if (!s.memory.empty())
            std::cout << theme::kv("Memory", s.memory);
        if (s.cpus_per_task > 0)
            std::cout << theme::kv("CPUs", std::to_string(s.cpus_per_task));
        if (!s.time.empty())
            std::cout << theme::kv("Time", s.time);
    }

    std::cout << "\n";
}

void do_info(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.job_manager()) return;

    std::string job_name = resolve_job_name(cli, arg);
    if (job_name.empty()) return;

    auto* tj = cli.service.find_job_by_name(job_name);
    if (!tj) {
        std::cout << theme::error(fmt::format("No tracked job named '{}'", job_name));
        return;
    }

    // Derive status string
    std::string status;
    if (!tj->init_error.empty() || (tj->canceled && !tj->init_complete))
        status = "ABORTED";
    else if (tj->canceled)
        status = "CANCELED";
    else if (tj->completed)
        status = tj->exit_code == 0 ? "COMPLETED" : fmt::format("FAILED (exit {})", tj->exit_code);
    else if (!tj->init_complete)
        status = "INITIALIZING";
    else
        status = "RUNNING";

    std::cout << "\n";
    std::cout << theme::kv("Job", job_name);
    std::cout << theme::kv("Status", status);

    if (!tj->slurm_id.empty())
        std::cout << theme::kv("Slurm ID", tj->slurm_id);
    if (!tj->compute_node.empty())
        std::cout << theme::kv("Node", tj->compute_node);

    // Timestamps
    if (!tj->submit_time.empty())
        std::cout << theme::kv("Submitted", format_timestamp(tj->submit_time));
    if (!tj->start_time.empty())
        std::cout << theme::kv("Started", format_timestamp(tj->start_time));
    if (!tj->end_time.empty())
        std::cout << theme::kv("Ended", format_timestamp(tj->end_time));

    // Wait time (submit → start)
    if (!tj->submit_time.empty() && !tj->start_time.empty())
        std::cout << theme::kv("Wait", format_duration(tj->submit_time, tj->start_time));

    // Duration (start → end, or start → now if running)
    if (!tj->start_time.empty())
        std::cout << theme::kv("Duration", format_duration(tj->start_time, tj->end_time));

    if (!tj->scratch_path.empty())
        std::cout << theme::kv("Scratch", tj->scratch_path);

    if (!tj->forwarded_ports.empty()) {
        std::string ports_str;
        for (int p : tj->forwarded_ports) {
            if (!ports_str.empty()) ports_str += ", ";
            ports_str += std::to_string(p);
        }
        std::cout << theme::kv("Ports", ports_str);
    }

    if (tj->completed && tj->exit_code >= 0)
        std::cout << theme::kv("Exit code", std::to_string(tj->exit_code));

    std::cout << "\n";
}

void register_jobs_commands(BaseCLI& cli) {
    cli.add_command("run", do_run, "Submit and attach to a job");
    cli.add_command("view", do_view, "Re-attach to a running job");
    cli.add_command("restart", do_restart, "Cancel and re-run a job");
    cli.add_command("output", do_output, "View full job output (vim)");
    cli.add_command("out", do_output, "View full job output (vim)");
    cli.add_command("logs", do_logs, "Print job output to terminal");
    cli.add_command("initlogs", do_initlogs, "Print job initialization logs");
    cli.add_command("ssh", do_ssh, "SSH to login node or job's compute node");
    cli.add_command("jobs", do_jobs, "List running jobs");
    cli.add_command("cancel", do_cancel, "Cancel a job");
    cli.add_command("return", do_return, "Download job output");
    cli.add_command("open", do_open, "Open an interactive remote shell");
    cli.add_command("clean", do_clean, "Remove old output (keeps latest)");
    cli.add_command("tail", do_tail, "Print last N lines of job output");
    cli.add_command("config", do_config, "Show project configuration");
    cli.add_command("info", do_info, "Show detailed job status");
}
