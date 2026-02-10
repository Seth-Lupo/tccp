#pragma once

#include <filesystem>
#include <mutex>
#include <core/types.hpp>
#include "result.hpp"

namespace fs = std::filesystem;

// libssh2 forward declaration
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

class SSHConnection {
public:
    explicit SSHConnection(LIBSSH2_CHANNEL* channel);

    SSHResult run(const std::string& command);
    SSHResult upload(const fs::path& local, const std::string& remote);
    SSHResult download(const std::string& remote, const fs::path& local);

    bool is_active() const;
    LIBSSH2_CHANNEL* get_channel() { return channel_; }

private:
    LIBSSH2_CHANNEL* channel_;
    std::recursive_mutex mutex_;
};
