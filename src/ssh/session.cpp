#include "session.hpp"
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <netinet/tcp.h>
#endif
#include <cstring>
#include <cerrno>
#include <memory>
#include <chrono>

// Data passed to keyboard-interactive callback via session abstract pointer
struct KbdAuthData {
    std::string password;
    bool use_duo;
    bool auto_duo;
    int prompt_round;
    StatusCallback callback;
};

// libssh2 keyboard-interactive callback
static void kbd_callback(const char* /*name*/, int /*name_len*/,
                          const char* /*instruction*/, int /*instruction_len*/,
                          int num_prompts,
                          const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                          LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                          void** abstract) {
    KbdAuthData* data = static_cast<KbdAuthData*>(*abstract);

    for (int i = 0; i < num_prompts; i++) {
        std::string prompt_text(reinterpret_cast<const char*>(prompts[i].text), prompts[i].length);

        if (prompt_text.find("assword") != std::string::npos) {
            // Password prompt
            if (data->callback) data->callback("Sending password...");
            responses[i].text = strdup(data->password.c_str());
            responses[i].length = data->password.length();
        } else if (prompt_text.find("option") != std::string::npos ||
                   prompt_text.find("passcode") != std::string::npos ||
                   prompt_text.find("Duo") != std::string::npos ||
                   prompt_text.find("factor") != std::string::npos) {
            // Duo 2FA prompt — send "1" for push
            if (data->callback) data->callback("Duo push sent, check your phone...");
            responses[i].text = strdup("1");
            responses[i].length = 1;
        } else {
            // Unknown prompt, try password as fallback
            responses[i].text = strdup(data->password.c_str());
            responses[i].length = data->password.length();
        }
    }
    data->prompt_round++;
}

SessionManager::SessionManager(const SessionTarget& target)
    : target_(target), session_(nullptr), channel_(nullptr), sock_(-1), active_(false),
      io_mutex_(std::make_shared<std::mutex>()) {
}

SessionManager::~SessionManager() {
    close();
}

SSHResult SessionManager::establish(StatusCallback callback) {
    return establish_connection(callback);
}

SSHResult SessionManager::establish_connection(StatusCallback callback) {
    if (callback) {
        callback("Connecting to " + target_.host + "...");
    }

    // Initialize libssh2
    int rc = libssh2_init(0);
    if (rc != 0) {
        return SSHResult{-1, "", "Failed to initialize libssh2"};
    }

    // Create socket
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        return SSHResult{-1, "", "Failed to create socket"};
    }

    // Set socket to non-blocking for libssh2
    platform::set_nonblocking(sock_);

    // Resolve and connect
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(22);

    if (inet_pton(AF_INET, target_.host.c_str(), &addr.sin_addr) != 1) {
        struct hostent* host_info = gethostbyname(target_.host.c_str());
        if (!host_info) {
            close();
            return SSHResult{-1, "", "Failed to resolve host: " + target_.host};
        }
        std::memcpy(&addr.sin_addr, host_info->h_addr, sizeof(in_addr));
    }

    int ret = connect(sock_, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        platform::close_socket(sock_);
        sock_ = -1;
        return SSHResult{-1, "", "Failed to connect: " + std::string(strerror(errno))};
    }

    // Wait for non-blocking connect to complete
    if (ret < 0) {
        int revents = platform::poll_socket(sock_, POLLOUT, target_.timeout * 1000);
        if (revents == 0) {
            platform::close_socket(sock_);
            sock_ = -1;
            return SSHResult{-1, "", "Connection timed out: " + target_.host};
        }
        int sock_err = 0;
        socklen_t err_len = sizeof(sock_err);
        getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sock_err), &err_len);
        if (sock_err != 0) {
            platform::close_socket(sock_);
            sock_ = -1;
            return SSHResult{-1, "", "Connection failed: " + std::string(strerror(sock_err))};
        }
    }

    if (callback) callback("TCP connected, starting SSH handshake...");

    // Create SSH session
    session_ = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    if (!session_) {
        platform::close_socket(sock_);
        sock_ = -1;
        return SSHResult{-1, "", "Failed to create SSH session"};
    }

    libssh2_session_set_blocking(session_, 0);

    // SSH handshake (key exchange)
    while ((ret = libssh2_session_handshake(session_, sock_)) == LIBSSH2_ERROR_EAGAIN) {
        platform::sleep_ms(100);
    }

    if (ret != 0) {
        libssh2_session_disconnect(session_, "Handshake failed");
        libssh2_session_free(session_);
        session_ = nullptr;
        platform::close_socket(sock_);
        sock_ = -1;
        return SSHResult{-1, "", "SSH handshake failed"};
    }

    // Enable TCP keepalive on the socket
    int tcp_keepalive = 1;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&tcp_keepalive), sizeof(tcp_keepalive));
    int keepidle = 60;
    int keepintvl = 15;
    int keepcnt = 4;
#ifdef TCP_KEEPIDLE
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<const char*>(&keepidle), sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<const char*>(&keepintvl), sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char*>(&keepcnt), sizeof(keepcnt));
#endif

    // Enable SSH keepalive (send every 30s, allow 3 missed)
    libssh2_keepalive_config(session_, 1, 30);

    if (callback) callback("SSH handshake complete, authenticating...");

    // SSH user authentication (transport level)
    auto auth_result = ssh_userauth(callback);
    if (auth_result.failed()) {
        libssh2_session_disconnect(session_, "Authentication failed");
        libssh2_session_free(session_);
        session_ = nullptr;
        platform::close_socket(sock_);
        sock_ = -1;
        return auth_result;
    }

    // Open channel (now that we're authenticated)
    while ((channel_ = libssh2_channel_open_session(session_)) == nullptr) {
        if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
            libssh2_session_disconnect(session_, "Failed to open channel");
            libssh2_session_free(session_);
            session_ = nullptr;
            platform::close_socket(sock_);
            sock_ = -1;
            return SSHResult{-1, "", "Failed to open SSH channel"};
        }
        platform::sleep_ms(100);
    }

    // Request PTY with actual terminal dimensions
    int tw = platform::term_width();
    int th = platform::term_height();
    while ((ret = libssh2_channel_request_pty_ex(
                channel_, "xterm", 5, nullptr, 0, tw, th, 0, 0)) == LIBSSH2_ERROR_EAGAIN) {
        platform::sleep_ms(100);
    }

    // Request shell
    while ((ret = libssh2_channel_shell(channel_)) == LIBSSH2_ERROR_EAGAIN) {
        platform::sleep_ms(100);
    }
    if (ret != 0) {
        libssh2_channel_close(channel_);
        libssh2_channel_free(channel_);
        channel_ = nullptr;
        libssh2_session_disconnect(session_, "Failed to request shell");
        libssh2_session_free(session_);
        session_ = nullptr;
        platform::close_socket(sock_);
        sock_ = -1;
        return SSHResult{-1, "", "Failed to request shell"};
    }

    // Wait for shell prompt
    auto prompt_result = wait_for_prompt(channel_, callback);
    if (prompt_result.failed()) {
        libssh2_channel_close(channel_);
        libssh2_channel_free(channel_);
        channel_ = nullptr;
        libssh2_session_disconnect(session_, "Shell not ready");
        libssh2_session_free(session_);
        session_ = nullptr;
        platform::close_socket(sock_);
        sock_ = -1;
        return prompt_result;
    }

    active_ = true;
    target_str_ = target_.user + "@" + target_.host;

    if (callback) {
        callback("Connected to " + target_.host);
    }

    return SSHResult{0, "", ""};
}

SSHResult SessionManager::ssh_userauth(StatusCallback callback) {
    int ret;

    // Check what auth methods the server supports
    char* auth_list = nullptr;
    while ((auth_list = libssh2_userauth_list(session_, target_.user.c_str(), target_.user.length())) == nullptr) {
        if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
            break;
        }
        platform::sleep_ms(100);
    }

    std::string methods = auth_list ? auth_list : "";
    if (callback && !methods.empty()) {
        callback("Auth methods: " + methods);
    }

    // Try keyboard-interactive (needed for Duo 2FA)
    if (methods.find("keyboard-interactive") != std::string::npos) {
        if (callback) callback("Using keyboard-interactive auth...");

        // Set up callback data via session abstract pointer
        KbdAuthData kbd_data;
        kbd_data.password = target_.password;
        kbd_data.use_duo = target_.use_duo;
        kbd_data.auto_duo = target_.auto_duo;
        kbd_data.prompt_round = 0;
        kbd_data.callback = callback;

        *libssh2_session_abstract(session_) = &kbd_data;

        while ((ret = libssh2_userauth_keyboard_interactive(session_,
                target_.user.c_str(), kbd_callback)) == LIBSSH2_ERROR_EAGAIN) {
            platform::sleep_ms(100);
        }

        if (ret == 0) {
            if (callback) callback("Authentication successful");
            return SSHResult{0, "", ""};
        }

        // keyboard-interactive failed, try password fallback
        if (callback) callback("Keyboard-interactive failed, trying password...");
    }

    // Try password auth
    if (methods.empty() || methods.find("password") != std::string::npos) {
        if (callback) callback("Using password auth...");

        while ((ret = libssh2_userauth_password(session_,
                target_.user.c_str(), target_.password.c_str())) == LIBSSH2_ERROR_EAGAIN) {
            platform::sleep_ms(100);
        }

        if (ret == 0) {
            if (callback) callback("Authentication successful");
            return SSHResult{0, "", ""};
        }
    }

    return SSHResult{-1, "", "Authentication failed (check username/password)"};
}

SSHResult SessionManager::wait_for_prompt(LIBSSH2_CHANNEL* chan, StatusCallback callback) {
    if (callback) callback("Waiting for shell...");

    ShellNegotiator negotiator;

    // Configure based on what this session might encounter
    if (target_.use_duo) {
        negotiator.set_duo_response("1");
    }

    // Password might be re-prompted in the shell (e.g. post-auth Duo flow)
    if (!target_.password.empty()) {
        negotiator.set_password(target_.password);
    }

    return negotiator.negotiate(chan, callback);
}

void SessionManager::close() {
    // Mark inactive first so concurrent operations bail out early
    active_ = false;

    // Each libssh2 call gets its own brief lock — avoids blocking other
    // operations for the entire disconnect sequence (which does network I/O).
    if (channel_) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_channel_close(channel_);
        }
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_channel_free(channel_);
        }
        channel_ = nullptr;
    }

    if (session_) {
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_session_disconnect(session_, "Normal disconnection");
        }
        {
            std::lock_guard<std::mutex> lock(*io_mutex_);
            libssh2_session_free(session_);
        }
        session_ = nullptr;
    }

    if (sock_ >= 0) {
        platform::close_socket(sock_);
        sock_ = -1;
    }
}

bool SessionManager::is_active() const {
    return active_;
}

bool SessionManager::check_alive() {
    std::lock_guard<std::mutex> lock(*io_mutex_);
    if (!active_ || !session_ || sock_ < 0) return false;

    // Send SSH keepalive and check if connection is still up
    int seconds_to_next = 0;
    int ret = libssh2_keepalive_send(session_, &seconds_to_next);
    if (ret != 0) {
        active_ = false;
        return false;
    }

    // Also check if the socket is still valid
    int revents = platform::poll_socket(sock_, POLLIN, 0);
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        active_ = false;
        return false;
    }

    return true;
}

void SessionManager::send_keepalive() {
    std::lock_guard<std::mutex> lock(*io_mutex_);
    if (session_ && active_) {
        int seconds_to_next = 0;
        libssh2_keepalive_send(session_, &seconds_to_next);
    }
}
