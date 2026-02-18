#pragma once

#include <string>
#include <vector>
#include <memory>
#include <core/types.hpp>
#include <ssh/connection.hpp>
#include <platform/process.hpp>

class PortForwarder {
public:
    explicit PortForwarder(const DTNConfig& dtn);

    // Ensure local SSH key exists and is authorized on DTN.
    // Returns true if system SSH to DTN works.
    bool ensure_keys(SSHConnection& dtn, StatusCallback log = nullptr);

    // Start SSH tunnels: localhost:port → compute_node:port via DTN.
    // Returns handles to spawned ssh processes.
    std::vector<std::shared_ptr<platform::ProcessHandle>> start(
        const std::string& compute_node,
        const std::vector<int>& ports,
        StatusCallback log = nullptr);

    // Kill tunnel processes.
    static void stop(std::vector<std::shared_ptr<platform::ProcessHandle>>& handles);

    // Check if a local TCP port is accepting connections.
    static bool is_port_open(int port);

private:
    const DTNConfig& dtn_;
    std::string username_;
    bool keys_ready_ = false;
    std::string key_path_;

    // Spawn a two-hop SSH tunnel process.
    platform::ProcessHandle spawn_tunnel(int port,
                                         const std::string& compute_node,
                                         const std::string& dtn_host);

    // Wait for a tunnel to bind its local port (up to timeout_ms).
    bool wait_for_tunnel(platform::ProcessHandle& handle, int port, int timeout_ms,
                         StatusCallback log);
};
