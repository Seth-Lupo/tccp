#include "port_forwarder.hpp"
#include "job_log.hpp"
#include <ssh/connection_factory.hpp>
#include <platform/platform.hpp>
#include <fmt/format.h>
#include <libssh2.h>
#ifndef _WIN32
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <poll.h>
#  include <fcntl.h>
#endif

// ── TunnelHandle ──────────────────────────────────────────

TunnelHandle::~TunnelHandle() {
    stop.store(true);
    if (thread.joinable()) thread.join();
    if (listen_fd >= 0) {
#ifdef _WIN32
        closesocket(listen_fd);
#else
        close(listen_fd);
#endif
    }
}

// ── PortForwarder ─────────────────────────────────────────

PortForwarder::PortForwarder(ConnectionFactory& factory)
    : factory_(factory) {}

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

// ── Tunnel thread ─────────────────────────────────────────

// Forward data between a local TCP socket and a libssh2 direct-tcpip channel.
// Runs until the connection closes or stop_flag is set.
static void forward_connection(ConnectionFactory& factory,
                               int client_fd,
                               LIBSSH2_CHANNEL* ch,
                               std::atomic<bool>& stop_flag) {
    char buf[16384];
    auto mtx = factory.session_mutex();
    int sock = factory.raw_socket();

    while (!stop_flag.load()) {
        // Poll local client socket for incoming data (100ms timeout)
        struct pollfd pfd = {client_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);

        // local → channel
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) break;  // client closed

            std::lock_guard<std::recursive_mutex> lock(*mtx);
            int sent = 0;
            while (sent < n) {
                int w = libssh2_channel_write(ch, buf + sent, n - sent);
                if (w == LIBSSH2_ERROR_EAGAIN) {
                    platform::sleep_ms(1);
                    continue;
                }
                if (w < 0) goto done;
                sent += w;
            }
        }

        // channel → local (non-blocking read)
        {
            std::lock_guard<std::recursive_mutex> lock(*mtx);
            libssh2_session_set_blocking(factory.raw_session(), 0);
            int n = libssh2_channel_read(ch, buf, sizeof(buf));
            libssh2_session_set_blocking(factory.raw_session(), 1);

            if (n > 0) {
                int sent = 0;
                while (sent < n) {
                    int w = write(client_fd, buf + sent, n - sent);
                    if (w <= 0) goto done;
                    sent += w;
                }
            } else if (n == 0 || libssh2_channel_eof(ch)) {
                break;  // remote closed
            }
            // EAGAIN = no data, keep looping
        }
    }

done:
    // Close channel under mutex
    {
        std::lock_guard<std::recursive_mutex> lock(*mtx);
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
    }
    close(client_fd);
}

void PortForwarder::tunnel_thread(ConnectionFactory& factory,
                                   std::string compute_node, int port,
                                   int listen_fd, std::atomic<bool>& stop_flag) {
    std::vector<std::thread> conn_threads;

    while (!stop_flag.load()) {
        // Accept with timeout so we can check stop flag
        struct pollfd pfd = {listen_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &len);
        if (client < 0) continue;

        // Open direct-tcpip channel to compute_node:port
        LIBSSH2_CHANNEL* ch = factory.tunnel(compute_node, port);
        if (!ch) {
            tccp_log(fmt::format("PortForwarder: direct-tcpip to {}:{} failed", compute_node, port));
            close(client);
            continue;
        }

        // Spawn a forwarding thread for this connection
        conn_threads.emplace_back(forward_connection,
            std::ref(factory), client, ch, std::ref(stop_flag));
    }

    // Clean up connection threads
    for (auto& t : conn_threads) {
        if (t.joinable()) t.join();
    }
}

// ── Start/Stop ────────────────────────────────────────────

std::vector<std::shared_ptr<TunnelHandle>> PortForwarder::start(
    const std::string& compute_node,
    const std::vector<int>& ports,
    StatusCallback log) {
    auto emit = [&](const std::string& msg) {
        tccp_log(fmt::format("PortForwarder: {}", msg));
        if (log) log(msg);
    };

    std::vector<std::shared_ptr<TunnelHandle>> handles;
    if (ports.empty()) return handles;

    for (int port : ports) {
        if (is_port_open(port)) {
            emit(fmt::format("Port {} already in use — skipping", port));
            continue;
        }

        // Create listening socket on localhost:port
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            emit(fmt::format("socket() failed for port {}", port));
            continue;
        }

        int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            emit(fmt::format("bind() failed for port {}", port));
            close(listen_fd);
            continue;
        }

        if (listen(listen_fd, 8) < 0) {
            emit(fmt::format("listen() failed for port {}", port));
            close(listen_fd);
            continue;
        }

        auto handle = std::make_shared<TunnelHandle>();
        handle->listen_fd = listen_fd;
        handle->thread = std::thread(tunnel_thread,
            std::ref(factory_), compute_node, port, listen_fd, std::ref(handle->stop));

        // Wait briefly for the port to become available
        for (int i = 0; i < 10; i++) {
            platform::sleep_ms(100);
            if (is_port_open(port)) break;
        }

        if (is_port_open(port)) {
            emit(fmt::format("localhost:{} → {}:{} ready", port, compute_node, port));
            handles.push_back(std::move(handle));
        } else {
            emit(fmt::format("Port {} not responding after bind", port));
            handle->stop.store(true);
            handles.push_back(std::move(handle));  // still track for cleanup
        }
    }

    return handles;
}

void PortForwarder::stop(std::vector<std::shared_ptr<TunnelHandle>>& handles) {
    for (auto& h : handles) {
        if (h) h->stop.store(true);
    }
    // Destructor joins threads and closes sockets
    handles.clear();
}
