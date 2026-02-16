#pragma once

#include "../base_cli.hpp"
#include <managers/job_manager.hpp>
#include <string>

// Shared helpers used by job command files (jobs.cpp, job_view_cmd.cpp, job_logs.cpp)

std::string resolve_job_name(BaseCLI& cli, const std::string& arg);
std::string output_path_for(const std::string& job_id);
std::string dtach_sock_for(const std::string& job_id);
bool wait_or_detach(int ms);
bool attach_to_job(BaseCLI& cli, TrackedJob* tracked,
                   const std::string& job_name, bool replay,
                   bool in_alt_screen = true);
void draw_job_header(const std::string& job_name,
                     const std::string& status,
                     const std::string& slurm_id = "",
                     const std::string& node = "");

// Forward declarations for command handlers (used by register_jobs_commands)
void do_run(BaseCLI& cli, const std::string& arg);
void do_view(BaseCLI& cli, const std::string& arg);
void do_restart(BaseCLI& cli, const std::string& arg);
void do_tail(BaseCLI& cli, const std::string& arg);
void do_ssh(BaseCLI& cli, const std::string& arg);
void do_open(BaseCLI& cli, const std::string& arg);
void do_logs(BaseCLI& cli, const std::string& arg);
void do_clean(BaseCLI& cli, const std::string& arg);
