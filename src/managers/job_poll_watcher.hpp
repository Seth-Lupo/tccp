#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SessionMultiplexer;

class JobPollWatcher {
public:
    struct WatchTarget {
        std::string job_id;
        std::string compute_node;
        std::string scratch_path;
    };

    using CompletionCallback = std::function<void(const std::string& job_id)>;

    JobPollWatcher(SessionMultiplexer& mux, CompletionCallback on_complete);
    ~JobPollWatcher();

    bool start();
    void stop();
    void update_targets(std::vector<WatchTarget> targets);

private:
    void watcher_loop();
    std::string ssh_cmd(const std::string& node, const std::string& remote_cmd) const;
    void cleanup_control_sockets();

    SessionMultiplexer& mux_;
    CompletionCallback on_complete_;
    int channel_id_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex targets_mutex_;
    std::vector<WatchTarget> targets_;

    // Track nodes that have established ControlMaster sockets
    std::mutex nodes_mutex_;
    std::vector<std::string> known_nodes_;
};
