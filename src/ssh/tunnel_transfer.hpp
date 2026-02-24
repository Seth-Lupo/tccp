#pragma once

#include <string>
#include <vector>
#include <functional>
#include <ssh/connection_factory.hpp>

struct TransferResult {
    bool verified = false;
    std::string error;
};

class TunnelTransfer {
public:
    TunnelTransfer(ConnectionFactory& factory, SSHConnection& dtn);

    // Stream tar of files from base_dir through a direct-tcpip tunnel to
    // compute_node:scratch_path. Verifies probe_file MD5 after extraction.
    TransferResult send_files(const std::string& compute_node,
                              const std::string& scratch_path,
                              const std::filesystem::path& base_dir,
                              const std::vector<std::string>& files,
                              const std::string& probe_file,
                              const std::string& expected_md5);

private:
    ConnectionFactory& factory_;
    SSHConnection& dtn_;
    std::string username_;

    // Write all bytes to tunnel channel with EAGAIN retry.
    bool channel_write_all(LIBSSH2_CHANNEL* ch,
                           const void* data, size_t len);
};
