#pragma once

#include "base_cli.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>

namespace replxx { class Replxx; }

// Forward declarations for command registration
void register_connection_commands(BaseCLI& cli);
void register_jobs_commands(BaseCLI& cli);
void register_shell_commands(BaseCLI& cli);
void register_allocations_commands(BaseCLI& cli);

class TCCPCLI : public BaseCLI {
public:
    TCCPCLI();

    void run_connected_repl();
    void run_register(const std::string& path_arg = "");
    void run_setup();
    void run_manual(const std::string& topic = "");
    void run_command(const std::string& command, const std::vector<std::string>& args);

private:
    void register_all_commands();

    // ── Job monitor (background thread) ────────────────────────
    // Polls every ~10s via SSH for job state changes.
    // Prints notifications via replxx::write() so they appear even
    // while the user is idle at the prompt.
    void start_monitor(replxx::Replxx& rx);
    void stop_monitor();
    void monitor_loop();

    // Mark current tracked jobs as announced so the monitor
    // doesn't re-notify after user-initiated actions (run/cancel/view).
    void sync_snapshots();

    replxx::Replxx* rx_ptr_ = nullptr;
    std::string current_prompt_;   // cached for monitor thread to redraw
    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{false};
    std::atomic<bool> connection_lost_{false};
};
