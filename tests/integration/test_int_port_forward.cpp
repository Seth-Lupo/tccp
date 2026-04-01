#include "test_fixture.hpp"
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <managers/port_forwarder.hpp>

namespace {

// Simple TCP client: connect, send, recv, close
std::string tcp_echo(int port, const std::string& msg, int timeout_ms = 5000) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    send(fd, msg.data(), msg.size(), 0);

    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    close(fd);

    if (n <= 0) return "";
    return std::string(buf, n);
}

} // namespace

// ── Port forward connects and echoes data ────────────────────── ~10 min
TEST_F(ClusterTest, PortForwardConnects) {
    // Write tccp.yaml with ports
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "ports:\n"
          << "  - 9100\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:02:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }
    // Echo server script
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import socket, time\n"
          << "s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
          << "s.bind(('0.0.0.0', 9100)); s.listen(1)\n"
          << "s.settimeout(60)\n"
          << "conn, _ = s.accept(); data = conn.recv(1024); conn.sendall(data); conn.close()\n"
          << "s.close()\n"
          << "time.sleep(30)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Start port forwarding
    PortForwarder pf(*service_->cluster());
    auto handles = pf.start(tj->compute_node, {9100});
    ASSERT_FALSE(handles.empty());

    // Wait for tunnel to be ready
    for (int i = 0; i < 20; i++) {
        if (PortForwarder::is_port_open(9100)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(PortForwarder::is_port_open(9100)) << "Port 9100 not open after tunnel start";

    // Give the python server a moment to start listening
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::string reply = tcp_echo(9100, "PING");
    EXPECT_EQ(reply, "PING") << "Echo server did not reply correctly";

    PortForwarder::stop(handles);
    wait_job_completed("quick", 120);
}

// ── Port forward cleaned up after cancel ─────────────────────── ~10 min
TEST_F(ClusterTest, PortForwardCleanedUpAfterCancel) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "ports:\n"
          << "  - 9100\n"
          << "jobs:\n"
          << "  slow:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }
    {
        std::ofstream f((project_dir_ / "test_slow.py").string());
        f << "import socket, time\n"
          << "s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
          << "s.bind(('0.0.0.0', 9100)); s.listen(1)\n"
          << "time.sleep(300)\n";
    }

    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("slow", 300);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);

    PortForwarder pf(*service_->cluster());
    auto handles = pf.start(tj->compute_node, {9100});

    for (int i = 0; i < 20; i++) {
        if (PortForwarder::is_port_open(9100)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ASSERT_TRUE(PortForwarder::is_port_open(9100));

    // Cancel job and stop forwarding
    service_->cancel_job("slow");
    PortForwarder::stop(handles);
    wait_job_completed("slow", 60);

    EXPECT_FALSE(PortForwarder::is_port_open(9100))
        << "Port 9100 still open after cancel + stop";
}

// ── Multiple ports forwarded simultaneously ──────────────────── ~10 min
TEST_F(ClusterTest, MultiplePortsForwarded) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "ports:\n"
          << "  - 9100\n"
          << "  - 9101\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:02:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import socket, threading, time\n"
          << "def serve(port):\n"
          << "    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
          << "    s.bind(('0.0.0.0', port)); s.listen(1); s.settimeout(60)\n"
          << "    conn, _ = s.accept(); data = conn.recv(1024); conn.sendall(data); conn.close(); s.close()\n"
          << "threading.Thread(target=serve, args=(9100,), daemon=True).start()\n"
          << "threading.Thread(target=serve, args=(9101,), daemon=True).start()\n"
          << "time.sleep(60)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    PortForwarder pf(*service_->cluster());
    auto handles = pf.start(tj->compute_node, {9100, 9101});

    // Wait for both tunnels
    for (int i = 0; i < 20; i++) {
        if (PortForwarder::is_port_open(9100) && PortForwarder::is_port_open(9101)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    EXPECT_TRUE(PortForwarder::is_port_open(9100)) << "Port 9100 not open";
    EXPECT_TRUE(PortForwarder::is_port_open(9101)) << "Port 9101 not open";

    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::string r1 = tcp_echo(9100, "A");
    std::string r2 = tcp_echo(9101, "B");
    EXPECT_EQ(r1, "A") << "Port 9100 echo failed";
    EXPECT_EQ(r2, "B") << "Port 9101 echo failed";

    PortForwarder::stop(handles);
    wait_job_completed("quick", 120);
}
