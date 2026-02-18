#pragma once

#include <memory>
#include <core/config.hpp>
#include "session.hpp"
#include "connection.hpp"
#include "expect.hpp"

// libssh2 forward declaration
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

class ClusterConnection {
public:
    explicit ClusterConnection(const Config& config);
    ~ClusterConnection();

    SSHResult connect(StatusCallback callback = nullptr);
    SSHResult disconnect();
    bool is_connected() const;
    bool check_alive();

    SSHConnection& dtn();
    SSHConnection& login();
    SessionManager* get_session() { return session_.get(); }

private:
    const Config& config_;
    std::unique_ptr<SessionManager> session_;
    LIBSSH2_CHANNEL* dtn_channel_;
    LIBSSH2_CHANNEL* login_channel_;
    std::unique_ptr<SSHConnection> dtn_conn_;
    std::unique_ptr<SSHConnection> login_conn_;
    std::string cluster_password_;

    SSHResult connect_dtn(StatusCallback callback);
    SSHResult open_login_tunnel(StatusCallback callback);
};
