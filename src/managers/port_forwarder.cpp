#include "port_forwarder.hpp"
#include "job_log.hpp"
#include <core/utils.hpp>
#include <fmt/format.h>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace fs = std::filesystem;

PortForwarder::PortForwarder(const DTNConfig& dtn)
    : dtn_(dtn), username_(get_cluster_username()) {
    const char* home = std::getenv("HOME");
    if (home) key_path_ = std::string(home) + "/.ssh/id_ed25519";
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
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int rc = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(sock);
    return rc == 0;
}

// ── Tunnel lifecycle ──────────────────────────────────────

pid_t PortForwarder::spawn_tunnel(int port,
                                   const std::string& compute_node,
                                   const std::string& dtn_host) {
    pid_t pid = fork();
    if (pid != 0) return pid;  // parent or error

    // Child: set up and exec SSH
    close(STDIN_FILENO);

    // Capture SSH errors to log file
    int fd = open("/tmp/tccp_tunnel.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }

    // Two-hop tunnel so we reach localhost on the compute node.
    // Servers inside Singularity containers often bind to 127.0.0.1, which
    // is not reachable via the node's external IP.
    //
    // Hop 1 (local → DTN):  ssh -L port:localhost:port user@DTN
    //   Binds local:port, forwards to DTN:localhost:port
    // Hop 2 (DTN → compute): ssh -N -L localhost:port:localhost:port compute
    //   Binds DTN:localhost:port, forwards to compute:localhost:port
    //
    // Chain: local:port → DTN:port → compute:localhost:port
    // DTN→compute uses cluster-internal auth (GSSAPI/hostbased).
    std::string local_fwd = fmt::format("{}:localhost:{}", port, port);
    std::string inner_cmd = fmt::format(
        "exec ssh -N -o StrictHostKeyChecking=no -o BatchMode=yes "
        "-L localhost:{}:localhost:{} {}", port, port, compute_node);

    execlp("ssh", "ssh",
           "-T",
           "-L", local_fwd.c_str(),
           "-o", "StrictHostKeyChecking=no",
           "-o", "BatchMode=yes",
           "-o", "ExitOnForwardFailure=yes",
           "-o", "ServerAliveInterval=60",
           "-o", "ServerAliveCountMax=3",
           "-i", key_path_.c_str(),
           dtn_host.c_str(),
           inner_cmd.c_str(),
           nullptr);
    _exit(127);
}

bool PortForwarder::wait_for_tunnel(pid_t pid, int port, int timeout_ms,
                                     StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        usleep(200000);
        elapsed += 200;

        // Check if process died
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            emit(fmt::format("Tunnel died (exit {}) — see /tmp/tccp_tunnel.log",
                             WEXITSTATUS(status)));
            return false;
        }

        if (is_port_open(port)) return true;
    }

    // Timed out — kill the zombie process
    emit(fmt::format("Tunnel timeout ({}s) — port {} never opened",
                     timeout_ms / 1000, port));
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

std::vector<pid_t> PortForwarder::start(const std::string& compute_node,
                                         const std::vector<int>& ports,
                                         StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    std::vector<pid_t> pids;
    if (!keys_ready_) { emit("Skipping — SSH keys not ready"); return pids; }
    if (ports.empty()) return pids;

    std::string dtn_host = fmt::format("{}@{}", username_, dtn_.host);

    for (int port : ports) {
        if (is_port_open(port)) {
            emit(fmt::format("Port {} already in use — skipping", port));
            continue;
        }

        emit(fmt::format("Forwarding localhost:{} → {}:{}...", port, compute_node, port));

        pid_t pid = spawn_tunnel(port, compute_node, dtn_host);
        if (pid < 0) {
            emit(fmt::format("fork failed for port {}", port));
            continue;
        }

        if (wait_for_tunnel(pid, port, 10000, log)) {
            pids.push_back(pid);
            emit(fmt::format("localhost:{} → {}:{} ready", port, compute_node, port));
        }
    }

    return pids;
}

void PortForwarder::stop(const std::vector<pid_t>& pids) {
    for (pid_t pid : pids) {
        if (pid <= 0) continue;
        if (kill(pid, SIGTERM) != 0) continue;

        // Wait up to 2s for graceful exit
        for (int i = 0; i < 20; i++) {
            int status;
            if (waitpid(pid, &status, WNOHANG) == pid) return;
            usleep(100000);
        }

        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
}
