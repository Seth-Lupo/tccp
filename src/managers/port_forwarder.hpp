#pragma once

#include <string>
#include <vector>
#include <core/types.hpp>
#include <ssh/connection.hpp>
#include <sys/types.h>

class PortForwarder {
public:
    explicit PortForwarder(const DTNConfig& dtn);

    // Ensure local SSH key exists and is authorized on DTN.
    // Returns true if system SSH to DTN works.
    bool ensure_keys(SSHConnection& dtn, StatusCallback log = nullptr);

    // Start SSH tunnels: localhost:port → compute_node:port via DTN.
    // Returns PIDs of spawned ssh processes.
    std::vector<pid_t> start(const std::string& compute_node,
                             const std::vector<int>& ports,
                             StatusCallback log = nullptr);

    // Kill tunnel processes by PID.
    static void stop(const std::vector<pid_t>& pids);

    // Check if a local TCP port is accepting connections.
    static bool is_port_open(int port);

private:
    const DTNConfig& dtn_;
    std::string username_;
    bool keys_ready_ = false;
    std::string key_path_;

    // Fork a two-hop SSH tunnel process. Returns pid, or -1 on failure.
    pid_t spawn_tunnel(int port,
                       const std::string& compute_node,
                       const std::string& dtn_host);

    // Wait for a tunnel to bind its local port (up to timeout_ms).
    // Returns true if port opened, false if process died or timed out.
    bool wait_for_tunnel(pid_t pid, int port, int timeout_ms,
                         StatusCallback log);
};
