#pragma once

#include "base_cli.hpp"
#include <string>
#include <vector>
#include <chrono>

// Forward declarations for command registration
void register_connection_commands(BaseCLI& cli);
void register_jobs_commands(BaseCLI& cli);
void register_shell_commands(BaseCLI& cli);
void register_setup_commands(BaseCLI& cli);
void register_credentials_commands(BaseCLI& cli);

class TCCPCLI : public BaseCLI {
public:
    TCCPCLI();

    void run_connected_repl();
    void run_register();
    void run_new(const std::string& template_name);
    void run_setup();
    void run_command(const std::string& command, const std::vector<std::string>& args);

private:
    void register_all_commands();

    std::chrono::steady_clock::time_point last_poll_time_{};
};
