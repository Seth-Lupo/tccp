#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <core/types.hpp>
#include "result.hpp"
#include "auth.hpp"

// libssh2 forward declarations
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

struct SessionTarget {
    std::string host;
    std::string user;
    std::string password;
    int timeout = 30;
    std::optional<std::string> ssh_key_path;
    bool use_duo = false;
    bool auto_duo = false;
};

class SessionManager {
public:
    explicit SessionManager(const SessionTarget& target);
    ~SessionManager();

    SSHResult establish(StatusCallback callback = nullptr);
    void close();
    bool is_active() const;
    bool check_alive();
    void send_keepalive();

    LIBSSH2_SESSION* get_raw_session() { return session_; }
    LIBSSH2_CHANNEL* get_channel() { return channel_; }
    int get_socket() const { return sock_; }
    const std::string& get_target() const { return target_str_; }
    std::shared_ptr<std::mutex> io_mutex() { return io_mutex_; }

private:
    SessionTarget target_;
    LIBSSH2_SESSION* session_;
    LIBSSH2_CHANNEL* channel_;
    int sock_;
    bool active_;
    std::string target_str_;
    std::shared_ptr<std::mutex> io_mutex_;

    SSHResult establish_connection(StatusCallback callback);
    SSHResult ssh_userauth(StatusCallback callback);
    SSHResult wait_for_prompt(LIBSSH2_CHANNEL* channel, StatusCallback callback);
};
