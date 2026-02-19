#pragma once

#include "base_cli.hpp"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

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
    void run_new(const std::string& template_name);
    void run_setup();
    void run_manual(const std::string& topic = "");
    void run_command(const std::string& command, const std::vector<std::string>& args);

private:
    void register_all_commands();

    // ── Job monitor (background thread) ────────────────────────
    // Runs every ~2s checking for init-thread state transitions.
    // Prints notifications via replxx::write() so they appear even
    // while the user is idle at the prompt.
    void start_monitor(replxx::Replxx& rx);
    void stop_monitor();
    void monitor_loop();

    // Collect notification strings from job state transitions.
    // Thread-safe: uses tracked_jobs_copy() + snapshot_mutex_.
    // Does NOT do SSH — purely local state diff.
    std::vector<std::string> collect_job_events();

    // Sync snapshots to current job state (prevents re-notifying
    // after user-initiated actions like run/cancel/view).
    void sync_snapshots();

    // Full SSH poll: detect remote job completion, then return output.
    // If out_msgs is provided, collect output messages instead of printing.
    void poll_and_return_output(std::vector<std::string>* out_msgs = nullptr);

    replxx::Replxx* rx_ptr_ = nullptr;
    std::string current_prompt_;   // cached for monitor thread to redraw
    std::thread monitor_thread_;
    std::atomic<bool> monitor_running_{false};
    std::atomic<bool> connection_lost_{false};

    // Snapshot of job states for detecting transitions between checks
    struct JobSnapshot {
        bool init_complete = false;
        bool completed = false;
        bool canceled = false;
        bool has_init_error = false;
        bool output_returned = false;
    };
    std::map<std::string, JobSnapshot> job_snapshots_;
    std::mutex snapshot_mutex_;
};
