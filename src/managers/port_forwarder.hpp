#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <core/types.hpp>

class ConnectionFactory;

// A single active tunnel: listen socket + forwarding thread.
struct TunnelHandle {
    std::atomic<bool> stop{false};
    std::thread thread;
    int listen_fd = -1;

    ~TunnelHandle();

    // Non-copyable, non-movable (thread + atomic)
    TunnelHandle() = default;
    TunnelHandle(const TunnelHandle&) = delete;
    TunnelHandle& operator=(const TunnelHandle&) = delete;
};

class PortForwarder {
public:
    explicit PortForwarder(ConnectionFactory& factory);

    // Start tunnels: localhost:port → compute_node:port via libssh2 direct-tcpip.
    std::vector<std::shared_ptr<TunnelHandle>> start(
        const std::string& compute_node,
        const std::vector<int>& ports,
        StatusCallback log = nullptr);

    // Stop tunnel threads and close sockets.
    static void stop(std::vector<std::shared_ptr<TunnelHandle>>& handles);

    // Check if a local TCP port is accepting connections.
    static bool is_port_open(int port);

private:
    ConnectionFactory& factory_;

    // Listen on localhost:port, forward through libssh2 direct-tcpip to target.
    static void tunnel_thread(ConnectionFactory& factory,
                              std::string compute_node, int port,
                              int listen_fd, std::atomic<bool>& stop_flag);
};
