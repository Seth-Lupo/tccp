#include "job_poll_watcher.hpp"
#include "job_log.hpp"
#include <ssh/session_multiplexer.hpp>
#include <core/constants.hpp>
#include <fmt/format.h>
#include <map>
#include <set>
#include <cstdlib>

// ── Construction / Destruction ──────────────────────────────

JobPollWatcher::JobPollWatcher(SessionMultiplexer& mux, CompletionCallback on_complete)
    : mux_(mux), on_complete_(std::move(on_complete)) {}

JobPollWatcher::~JobPollWatcher() {
    stop();
}

// ── Lifecycle ───────────────────────────────────────────────

bool JobPollWatcher::start() {
    if (running_) return true;

    channel_id_ = mux_.open_channel();
    if (channel_id_ < 0) {
        tccp_log("poll_watcher: failed to open multiplexed channel, falling back to SSH poll");
        return false;
    }

    running_ = true;
    thread_ = std::thread(&JobPollWatcher::watcher_loop, this);
    tccp_log("poll_watcher: started on channel " + std::to_string(channel_id_));
    return true;
}

void JobPollWatcher::stop() {
    if (!running_) return;

    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }

    if (channel_id_ >= 0) {
        mux_.close_channel(channel_id_);
        channel_id_ = -1;
    }

    cleanup_control_sockets();
    tccp_log("poll_watcher: stopped");
}

void JobPollWatcher::update_targets(std::vector<WatchTarget> targets) {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    targets_ = std::move(targets);
}

// ── Watcher loop ────────────────────────────────────────────

void JobPollWatcher::watcher_loop() {
    while (running_) {
        // Copy targets
        std::vector<WatchTarget> targets;
        {
            std::lock_guard<std::mutex> lock(targets_mutex_);
            targets = targets_;
        }

        if (!targets.empty()) {
            // Group by compute node
            std::map<std::string, std::vector<const WatchTarget*>> by_node;
            for (const auto& t : targets) {
                if (!t.compute_node.empty()) {
                    by_node[t.compute_node].push_back(&t);
                }
            }

            // Batch check per node via ControlMaster SSH
            for (const auto& [node, node_targets] : by_node) {
                if (!running_) break;

                std::string batch;
                for (const auto* t : node_targets) {
                    if (!batch.empty()) batch += " ";
                    batch += fmt::format(
                        "test -e {}/tccp.sock && echo {}:RUN || echo {}:DONE;",
                        t->scratch_path, t->job_id, t->job_id);
                }

                auto result = mux_.run(channel_id_, ssh_cmd(node, batch));

                // Track node for cleanup
                {
                    std::lock_guard<std::mutex> lock(nodes_mutex_);
                    bool found = false;
                    for (const auto& n : known_nodes_) {
                        if (n == node) { found = true; break; }
                    }
                    if (!found) known_nodes_.push_back(node);
                }

                // Parse results
                for (const auto* t : node_targets) {
                    std::string done_marker = t->job_id + ":DONE";
                    if (result.stdout_data.find(done_marker) != std::string::npos) {
                        tccp_log(fmt::format("poll_watcher: job {} completed on {}",
                                             t->job_id, node));
                        on_complete_(t->job_id);
                    }
                }
            }
        }

        // Sleep 2 seconds in 100ms increments for responsive shutdown
        for (int i = 0; i < 20 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ── SSH helpers ─────────────────────────────────────────────

std::string JobPollWatcher::ssh_cmd(const std::string& node,
                                     const std::string& remote_cmd) const {
    return fmt::format(
        "ssh -o StrictHostKeyChecking=no -o BatchMode=yes -o LogLevel=ERROR "
        "-o ConnectTimeout=3 "
        "-o ControlMaster=auto -o ControlPath=/tmp/tccp_poll_%h "
        "-o ControlPersist=300 "
        "{} '{}'",
        node, remote_cmd);
}

void JobPollWatcher::cleanup_control_sockets() {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    for (const auto& node : known_nodes_) {
        std::string cmd = fmt::format(
            "ssh -o ControlPath=/tmp/tccp_poll_%h -O exit {} 2>/dev/null", node);
        std::system(cmd.c_str());
    }
    known_nodes_.clear();
}
