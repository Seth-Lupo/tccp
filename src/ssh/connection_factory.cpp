#include "connection_factory.hpp"
#include "session_multiplexer.hpp"
#include "multiplexed_connection.hpp"
#include "multiplexed_login_hop.hpp"
#include "multiplexed_shell.hpp"
#include <libssh2.h>
#include <platform/platform.hpp>
#include <core/credentials.hpp>
#include <fmt/format.h>

// ── Lifecycle ──────────────────────────────────────────────────

ConnectionFactory::ConnectionFactory(const Config& config)
    : config_(config) {}

ConnectionFactory::~ConnectionFactory() {
    disconnect();
}

SSHResult ConnectionFactory::connect(StatusCallback callback) {
    auto result = connect_session(callback);
    if (result.failed()) return result;

    // Start the session multiplexer (tmux -CC on primary channel)
    mux_ = std::make_unique<SessionMultiplexer>(
        session_->get_channel(),
        session_->get_raw_session(),
        session_->io_mutex(),
        session_->get_socket());

    auto mux_result = mux_->start();
    if (mux_result.failed()) {
        std::string err = "Failed to start session multiplexer";
        if (!mux_result.stderr_data.empty())
            err += ": " + mux_result.stderr_data;
        if (!mux_result.stdout_data.empty())
            err += " [output: " + mux_result.stdout_data + "]";
        mux_.reset();
        session_->close();
        session_.reset();
        return SSHResult{-1, "", err};
    }

    if (callback)
        callback("[cluster] Session multiplexer started");

    // DTN connection: dedicated multiplexed channel for DTN commands
    int dtn_ch = mux_->open_channel();
    if (dtn_ch < 0) {
        mux_->stop();
        mux_.reset();
        session_->close();
        session_.reset();
        return SSHResult{-1, "", "Failed to open DTN channel"};
    }

    dtn_ = std::make_unique<MultiplexedConnection>(
        *mux_, dtn_ch,
        session_->get_raw_session(),
        session_->io_mutex(),
        session_->get_socket());

    // Login hop: dedicated multiplexed channel (parallel with DTN commands)
    int login_ch = mux_->open_channel();
    if (login_ch < 0) {
        mux_->stop();
        mux_.reset();
        session_->close();
        session_.reset();
        return SSHResult{-1, "", "Failed to open login channel"};
    }

    login_ = std::make_unique<MultiplexedLoginHop>(
        *mux_, login_ch,
        session_->get_raw_session(),
        session_->io_mutex(),
        session_->get_socket(),
        config_.login().host);

    if (callback)
        callback("[cluster] DTN + Login hop connections established (single auth)");

    return SSHResult{0, "", ""};
}

SSHResult ConnectionFactory::disconnect() {
    login_.reset();
    dtn_.reset();
    if (mux_) {
        mux_->stop();
        mux_.reset();
    }
    if (session_) {
        session_->close();
        session_.reset();
    }
    return SSHResult{0, "Disconnected", ""};
}

bool ConnectionFactory::is_connected() const {
    return session_ && session_->is_active() &&
           mux_ && mux_->is_running() &&
           dtn_ && dtn_->is_active();
}

bool ConnectionFactory::check_alive() {
    if (!session_) return false;
    return session_->check_alive();
}

void ConnectionFactory::send_keepalive() {
    if (session_) session_->send_keepalive();
}

// ── Connection grants ──────────────────────────────────────────

SSHConnection& ConnectionFactory::dtn() { return *dtn_; }
SSHConnection& ConnectionFactory::login() { return *login_; }

MultiplexedShell ConnectionFactory::shell() {
    return MultiplexedShell(*this);
}

// ── Channel grants ─────────────────────────────────────────────

LIBSSH2_CHANNEL* ConnectionFactory::exec_channel() {
    auto io_mtx = session_->io_mutex();
    LIBSSH2_SESSION* ssh = session_->get_raw_session();
    if (!ssh) return nullptr;

    LIBSSH2_CHANNEL* ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mtx);
            ch = libssh2_channel_open_session(ssh);
            if (!ch && libssh2_session_last_errno(ssh) != LIBSSH2_ERROR_EAGAIN)
                return nullptr;
        }
        if (ch) break;
        platform::sleep_ms(10);
    }
    return ch;
}

LIBSSH2_CHANNEL* ConnectionFactory::tunnel(const std::string& host, int port) {
    auto io_mtx = session_->io_mutex();
    LIBSSH2_SESSION* ssh = session_->get_raw_session();
    if (!ssh) return nullptr;

    LIBSSH2_CHANNEL* ch = nullptr;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*io_mtx);
            ch = libssh2_channel_direct_tcpip(ssh, host.c_str(), port);
            if (!ch && libssh2_session_last_errno(ssh) != LIBSSH2_ERROR_EAGAIN)
                return nullptr;
        }
        if (ch) break;
        platform::sleep_ms(10);
    }
    return ch;
}

// ── Raw access ─────────────────────────────────────────────────

LIBSSH2_CHANNEL* ConnectionFactory::primary_channel() {
    return session_ ? session_->get_channel() : nullptr;
}

LIBSSH2_SESSION* ConnectionFactory::raw_session() {
    return session_ ? session_->get_raw_session() : nullptr;
}

int ConnectionFactory::raw_socket() {
    return session_ ? session_->get_socket() : -1;
}

std::shared_ptr<std::mutex> ConnectionFactory::io_mutex() {
    return session_ ? session_->io_mutex() : nullptr;
}

SessionMultiplexer* ConnectionFactory::multiplexer() {
    return mux_.get();
}

// ── Internal ───────────────────────────────────────────────────

SSHResult ConnectionFactory::connect_session(StatusCallback callback) {
    if (callback)
        callback("[cluster] Connecting to DTN: " + config_.dtn().host);

    auto& creds = CredentialManager::instance();
    auto user_result = creds.get("user");
    auto pass_result = creds.get("password");

    if (user_result.is_err() || pass_result.is_err())
        return SSHResult{-1, "", "No credentials found. Run 'tccp setup' first."};

    SessionTarget target;
    target.host = config_.dtn().host;
    target.user = user_result.value;
    target.password = pass_result.value;
    target.timeout = config_.dtn().timeout;
    target.ssh_key_path = config_.dtn().ssh_key_path;
    target.use_duo = true;
    target.auto_duo = config_.duo_auto().has_value();

    session_ = std::make_unique<SessionManager>(target);

    auto result = session_->establish(callback);
    if (result.failed()) {
        session_.reset();
        return result;
    }

    if (callback)
        callback("[cluster] DTN connection established");

    return SSHResult{0, "", ""};
}
