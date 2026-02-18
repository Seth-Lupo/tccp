#include "cluster_connection.hpp"
#include <libssh2.h>
#include <fmt/format.h>
#include <core/credentials.hpp>

ClusterConnection::ClusterConnection(const Config& config)
    : config_(config), dtn_channel_(nullptr), login_channel_(nullptr) {
}

ClusterConnection::~ClusterConnection() {
    disconnect();
}

SSHResult ClusterConnection::connect(StatusCallback callback) {
    // Step 1: Connect to DTN
    auto dtn_result = connect_dtn(callback);
    if (dtn_result.failed()) {
        return dtn_result;
    }

    // Step 2: Open login tunnel through DTN
    auto login_result = open_login_tunnel(callback);
    if (login_result.failed()) {
        return login_result;
    }

    if (callback) {
        callback("[cluster] Both DTN and Login connections established");
    }

    return SSHResult{0, "", ""};
}

SSHResult ClusterConnection::disconnect() {
    login_conn_.reset();
    dtn_conn_.reset();

    // Close login channel if we opened it separately
    if (login_channel_) {
        libssh2_channel_close(login_channel_);
        libssh2_channel_free(login_channel_);
        login_channel_ = nullptr;
    }

    // Session close handles the primary channel and socket
    if (session_) {
        session_->close();
        session_.reset();
    }

    dtn_channel_ = nullptr;

    return SSHResult{0, "Disconnected", ""};
}

bool ClusterConnection::is_connected() const {
    return session_ && session_->is_active() &&
           dtn_conn_ && dtn_conn_->is_active() &&
           login_conn_ && login_conn_->is_active();
}

bool ClusterConnection::check_alive() {
    if (!session_) return false;
    return session_->check_alive();
}

SSHConnection& ClusterConnection::dtn() {
    return *dtn_conn_;
}

SSHConnection& ClusterConnection::login() {
    return *login_conn_;
}

SSHResult ClusterConnection::connect_dtn(StatusCallback callback) {
    if (callback) {
        callback("[cluster] Connecting to DTN: " + config_.dtn().host);
    }

    // Read credentials from keychain
    auto& creds = CredentialManager::instance();
    auto user_result = creds.get("user");
    auto pass_result = creds.get("password");

    if (user_result.is_err() || pass_result.is_err()) {
        return SSHResult{-1, "", "No credentials found. Run 'tccp setup' first."};
    }

    // Build SessionTarget from DTN config + keychain credentials
    SessionTarget dtn_target;
    dtn_target.host = config_.dtn().host;
    dtn_target.user = user_result.value;
    dtn_target.password = pass_result.value;
    dtn_target.timeout = config_.dtn().timeout;
    dtn_target.ssh_key_path = config_.dtn().ssh_key_path;
    dtn_target.use_duo = true;
    dtn_target.auto_duo = config_.duo_auto().has_value();

    cluster_password_ = pass_result.value;

    session_ = std::make_unique<SessionManager>(dtn_target);

    auto result = session_->establish(callback);
    if (result.failed()) {
        session_.reset();
        return result;
    }

    // The session's primary channel is our DTN channel
    dtn_channel_ = session_->get_channel();
    dtn_conn_ = std::make_unique<SSHConnection>(dtn_channel_, session_->get_raw_session());

    if (callback) {
        callback("[cluster] DTN connection established");
    }

    return SSHResult{0, "", ""};
}

SSHResult ClusterConnection::open_login_tunnel(StatusCallback callback) {
    if (callback) {
        callback("[cluster] Opening tunnel to Login: " + config_.login().host);
    }

    // Open a second channel on the same SSH session to DTN
    login_channel_ = session_->open_extra_channel(callback);
    if (!login_channel_) {
        return SSHResult{-1, "", "Failed to open channel for login tunnel"};
    }

    // The second channel gets its own DTN shell — may require Duo again
    {
        ShellNegotiator dtn_negotiator;
        dtn_negotiator.set_duo_response("1");
        if (!cluster_password_.empty()) {
            dtn_negotiator.set_password(cluster_password_);
        }
        auto dtn_result = dtn_negotiator.negotiate(login_channel_, callback);
        if (dtn_result.failed()) {
            return SSHResult{-1, "", "DTN shell on login channel failed: " + dtn_result.stderr_data};
        }
    }

    // Now we have a DTN shell — SSH to login node (passwordless internal hop)
    std::string ssh_cmd = "ssh " + config_.login().host + "\n";
    int written = libssh2_channel_write(login_channel_, ssh_cmd.c_str(), ssh_cmd.length());
    if (written < 0) {
        return SSHResult{-1, "", "Failed to send SSH tunnel command"};
    }

    if (callback) {
        callback("[cluster] Waiting for login shell...");
    }

    // Internal hop — just wait for shell prompt
    ShellNegotiator login_negotiator;
    login_negotiator.set_timeout(std::chrono::seconds(30));
    auto shell_result = login_negotiator.negotiate(login_channel_, callback);
    if (shell_result.failed()) {
        return shell_result;
    }

    login_conn_ = std::make_unique<SSHConnection>(login_channel_, session_->get_raw_session());

    if (callback) {
        callback("[cluster] Login tunnel established");
    }

    return SSHResult{0, "", ""};
}
