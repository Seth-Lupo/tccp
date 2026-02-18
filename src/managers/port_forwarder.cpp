#include "port_forwarder.hpp"
#include "job_log.hpp"
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <platform/process.hpp>
#include <fmt/format.h>
#include <fstream>
#include <filesystem>
#ifndef _WIN32
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

namespace fs = std::filesystem;

PortForwarder::PortForwarder(const DTNConfig& dtn)
    : dtn_(dtn), username_(get_cluster_username()) {
    key_path_ = (platform::home_dir() / ".ssh" / "id_ed25519").string();
}

// ── Key setup ─────────────────────────────────────────────

bool PortForwarder::ensure_keys(SSHConnection& dtn, StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    if (keys_ready_) return true;
    if (key_path_.empty()) { emit("HOME not set"); return false; }

    // Generate key if missing
    if (!fs::exists(key_path_)) {
        emit("Generating SSH key...");
        std::string cmd = fmt::format("ssh-keygen -t ed25519 -N \"\" -f {} -q", key_path_);
        if (std::system(cmd.c_str()) != 0) { emit("ssh-keygen failed"); return false; }
    }

    // Read public key
    std::ifstream pubfile(key_path_ + ".pub");
    std::string pubkey;
    if (pubfile) std::getline(pubfile, pubkey);
    if (pubkey.empty()) { emit("Cannot read public key"); return false; }

    // Escape single quotes for shell
    std::string escaped = pubkey;
    for (std::string::size_type p = 0; (p = escaped.find('\'', p)) != std::string::npos; p += 4)
        escaped.replace(p, 1, "'\\''");

    // Install on DTN via libssh2
    emit("Installing pubkey on DTN...");
    auto result = dtn.run(fmt::format(
        "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
        "grep -qF '{}' ~/.ssh/authorized_keys 2>/dev/null || "
        "echo '{}' >> ~/.ssh/authorized_keys && "
        "chmod 600 ~/.ssh/authorized_keys",
        escaped, escaped));
    if (result.failed()) {
        emit(fmt::format("Pubkey install failed: {}", result.get_output()));
        return false;
    }

    // Verify system SSH works
    emit("Verifying system SSH to DTN...");
    std::string verify = fmt::format(
        "ssh -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no "
        "-i {} {}@{} echo OK >/dev/null 2>&1",
        key_path_, username_, dtn_.host);
    if (std::system(verify.c_str()) != 0) {
        emit(fmt::format("System SSH verify failed ({}@{})", username_, dtn_.host));
        return false;
    }

    keys_ready_ = true;
    emit("SSH keys ready");
    return true;
}

// ── Port checking ─────────────────────────────────────────

bool PortForwarder::is_port_open(int port) {
#ifdef _WIN32
    platform::init_networking();
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    closesocket(sock);
    return rc == 0;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(sock);
    return rc == 0;
#endif
}

// ── Tunnel lifecycle ──────────────────────────────────────

platform::ProcessHandle PortForwarder::spawn_tunnel(int port,
                                                     const std::string& compute_node,
                                                     const std::string& dtn_host) {
    std::string tunnel_log = (platform::temp_dir() / "tccp_tunnel.log").string();
    std::string local_fwd = fmt::format("{}:localhost:{}", port, port);
    std::string inner_cmd = fmt::format(
        "exec ssh -N -o StrictHostKeyChecking=no -o BatchMode=yes "
        "-L localhost:{}:localhost:{} {}", port, port, compute_node);

    return platform::spawn("ssh", {
        "-T",
        "-L", local_fwd,
        "-o", "StrictHostKeyChecking=no",
        "-o", "BatchMode=yes",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ServerAliveInterval=60",
        "-o", "ServerAliveCountMax=3",
        "-i", key_path_,
        dtn_host,
        inner_cmd
    }, tunnel_log);
}

bool PortForwarder::wait_for_tunnel(platform::ProcessHandle& handle, int port,
                                     int timeout_ms, StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        platform::sleep_ms(200);
        elapsed += 200;

        // Check if process died
        if (!handle.running()) {
            emit(fmt::format("Tunnel died — see {}",
                             (platform::temp_dir() / "tccp_tunnel.log").string()));
            return false;
        }

        if (is_port_open(port)) return true;
    }

    // Timed out — kill the process
    emit(fmt::format("Tunnel timeout ({}s) — port {} never opened",
                     timeout_ms / 1000, port));
    handle.terminate();
    return false;
}

std::vector<std::shared_ptr<platform::ProcessHandle>> PortForwarder::start(
    const std::string& compute_node,
    const std::vector<int>& ports,
    StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    std::vector<std::shared_ptr<platform::ProcessHandle>> handles;
    if (!keys_ready_) { emit("Skipping — SSH keys not ready"); return handles; }
    if (ports.empty()) return handles;

    std::string dtn_host = fmt::format("{}@{}", username_, dtn_.host);

    for (int port : ports) {
        if (is_port_open(port)) {
            emit(fmt::format("Port {} already in use — skipping", port));
            continue;
        }

        emit(fmt::format("Forwarding localhost:{} → {}:{}...", port, compute_node, port));

        auto handle = std::make_shared<platform::ProcessHandle>(
            spawn_tunnel(port, compute_node, dtn_host));
        if (!handle->valid()) {
            emit(fmt::format("spawn failed for port {}", port));
            continue;
        }

        if (wait_for_tunnel(*handle, port, 10000, log)) {
            emit(fmt::format("localhost:{} → {}:{} ready", port, compute_node, port));
            handles.push_back(std::move(handle));
        }
    }

    return handles;
}

void PortForwarder::stop(std::vector<std::shared_ptr<platform::ProcessHandle>>& handles) {
    for (auto& handle : handles) {
        if (handle && handle->valid()) {
            handle->terminate();
        }
    }
}
