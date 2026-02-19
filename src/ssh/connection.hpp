#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <core/types.hpp>
#include "result.hpp"

namespace fs = std::filesystem;

// libssh2 forward declarations
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

class SSHConnection {
public:
    SSHConnection(LIBSSH2_CHANNEL* channel, LIBSSH2_SESSION* session,
                  std::shared_ptr<std::recursive_mutex> session_mutex);

    virtual ~SSHConnection() = default;

    virtual SSHResult run(const std::string& command, int timeout_secs = 0);
    SSHResult upload(const fs::path& local, const std::string& remote);
    SSHResult download(const std::string& remote, const fs::path& local);

    // Execute a command on a new exec channel and pipe binary data to its stdin.
    // Returns the command's stdout. Requires session_ to be set.
    SSHResult run_with_input(const std::string& command,
                             const char* data, size_t data_len,
                             int timeout_secs = 0);

    bool is_active() const;

protected:
    LIBSSH2_CHANNEL* channel_;
    LIBSSH2_SESSION* session_;
    std::shared_ptr<std::recursive_mutex> session_mutex_;
};
