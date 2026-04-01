#include "tunnel_transfer.hpp"
#include "compute_hop.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <platform/archive.hpp>
#include <fmt/format.h>
#include <sstream>
#include <libssh2.h>

namespace fs = std::filesystem;

TunnelTransfer::TunnelTransfer(ConnectionFactory& factory, SSHConnection& dtn)
    : factory_(factory), dtn_(dtn), username_(get_cluster_username()) {
}

bool TunnelTransfer::channel_write_all(LIBSSH2_CHANNEL* ch,
                                        const void* data, size_t len) {
    auto io_mtx = factory_.io_mutex();
    size_t sent = 0;
    int retries = 0;
    while (sent < len) {
        ssize_t w;
        {
            std::lock_guard<std::mutex> lock(*io_mtx);
            w = libssh2_channel_write(ch, static_cast<const char*>(data) + sent, len - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) {
            if (++retries > 5000) return false;
            platform::sleep_ms(1);
            continue;
        }
        if (w < 0) return false;
        sent += static_cast<size_t>(w);
        retries = 0;
    }
    return true;
}

static std::string parse_md5(const std::string& output) {
    std::string trimmed = output;
    trim(trimmed);
    std::istringstream iss(trimmed);
    std::string line;
    while (std::getline(iss, line)) {
        trim(line);
        if (line.size() == 32 &&
            line.find_first_not_of("0123456789abcdef") == std::string::npos) {
            return line;
        }
    }
    return "";
}

TransferResult TunnelTransfer::send_files(const std::string& compute_node,
                                           const std::string& scratch_path,
                                           const fs::path& base_dir,
                                           const std::vector<std::string>& files,
                                           const std::string& probe_file,
                                           const std::string& expected_md5) {
    // Markers on DTN (local, no SSH hop needed to read/check)
    std::string port_marker = fmt::format("/tmp/{}/.tccp-sync-port", username_);
    std::string done_marker = fmt::format("/tmp/{}/.tccp-sync-done", username_);

    // Clean up old markers
    dtn_.run(fmt::format("rm -f {} {}", port_marker, done_marker), 5);

    // Start background receiver on DTN: python TCP listener piped into
    // SSH hop to compute node where tar extracts. Runs as background
    // subshell so dtn_.run() returns immediately — avoids cmd_mutex_
    // deadlock. The listener binds on DTN localhost; we tunnel to it
    // via direct-tcpip (no Duo, no Kerberos needed).
    dtn_.run(fmt::format(
        "(python3 -c \""
        "import socket,sys,shutil;"
        "s=socket.socket();"
        "s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);"
        "s.bind(('127.0.0.1',0));"
        "s.listen(1);"
        "open('{}','w').write(str(s.getsockname()[1]));"
        "c,_=s.accept();"
        "shutil.copyfileobj(c.makefile('rb'),sys.stdout.buffer);"
        "c.close()"
        "\" | ssh -T {} {} 'mkdir -p {} && cd {} && tar xf -'"
        " && touch {} >/dev/null 2>&1 &)",
        port_marker,
        SSH_OPTS, compute_node,
        scratch_path, scratch_path,
        done_marker));

    // Poll for port marker on DTN (local file, fast)
    int port = 0;
    for (int attempt = 0; attempt < 25; ++attempt) {
        platform::sleep_ms(200);
        auto r = dtn_.run(fmt::format("cat {} 2>/dev/null", port_marker), 5);
        std::string out = r.stdout_data;
        trim(out);
        if (!out.empty()) {
            try { port = std::stoi(out); } catch (...) {}
            if (port > 0) break;
        }
    }

    if (port <= 0) {
        dtn_.run(fmt::format("rm -f {} {}", port_marker, done_marker), 5);
        return TransferResult{false, "Failed to read receiver port from marker file"};
    }

    // Open direct-tcpip tunnel to DTN localhost (where python listener runs)
    LIBSSH2_CHANNEL* tunnel = factory_.tunnel("127.0.0.1", port);
    if (!tunnel) {
        dtn_.run(fmt::format("rm -f {} {}", port_marker, done_marker), 5);
        return TransferResult{false, "Failed to open tunnel to receiver"};
    }

    // Stream tar through tunnel using libarchive callback
    bool write_ok = true;
    auto write_fn = [&](const void* buf, size_t len) -> int64_t {
        if (!write_ok) return -1;
        if (!channel_write_all(tunnel, buf, len)) {
            write_ok = false;
            return -1;
        }
        return static_cast<int64_t>(len);
    };

    try {
        platform::create_tar_to_callback(base_dir, files, write_fn);
    } catch (const std::exception& e) {
        {
            auto io_mtx = factory_.io_mutex();
            std::lock_guard<std::mutex> lock(*io_mtx);
            libssh2_channel_close(tunnel);
            libssh2_channel_free(tunnel);
        }
        dtn_.run(fmt::format("rm -f {} {}", port_marker, done_marker), 5);
        return TransferResult{false, fmt::format("Tar creation failed: {}", e.what())};
    }

    // Close tunnel → EOF → python exits → pipe closes → tar finishes → ssh exits
    {
        auto io_mtx = factory_.io_mutex();
        std::lock_guard<std::mutex> lock(*io_mtx);
        libssh2_channel_send_eof(tunnel);
        libssh2_channel_close(tunnel);
        libssh2_channel_free(tunnel);
    }

    // Wait for extraction to complete (done marker appears on DTN)
    bool done = false;
    for (int i = 0; i < 150; ++i) {  // up to 30s
        platform::sleep_ms(200);
        auto r = dtn_.run(fmt::format("test -f {} && echo yes", done_marker), 5);
        std::string out = r.stdout_data;
        trim(out);
        if (out == "yes") { done = true; break; }
    }

    // Cleanup markers
    dtn_.run(fmt::format("rm -f {} {}", port_marker, done_marker), 5);

    if (!done) {
        return TransferResult{false, "Extraction did not complete within timeout"};
    }

    // Verify MD5 on compute node
    ComputeHop compute(dtn_, compute_node);
    auto verify = compute.run(fmt::format(
        "cd {} && md5sum {} 2>/dev/null | cut -d\" \" -f1",
        scratch_path, probe_file), 30);

    std::string remote_md5 = parse_md5(verify.stdout_data);

    if (remote_md5 != expected_md5) {
        return TransferResult{false, fmt::format(
            "MD5 mismatch for {}: expected {}, got {}", probe_file, expected_md5, remote_md5)};
    }

    return TransferResult{true, ""};
}
