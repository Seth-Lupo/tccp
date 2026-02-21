// Experiment: Master shell + arbitrary user shells via tmux control mode.
// Window 0 = master (hidden). User shells get $INDEX env var.
//
// Keys:
//   Ctrl+T  — create new shell
//   Ctrl+W  — kill current shell
//   Ctrl+N  — next shell (wraps)
//   Ctrl+P  — previous shell (wraps)
//   Ctrl+C  — exit

#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/types.hpp>
#include <ssh/session.hpp>
#include <ssh/connection.hpp>
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <libssh2.h>
#include <fmt/format.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <vector>
#include <algorithm>

static void ch_write(LIBSSH2_CHANNEL* ch,
                     std::shared_ptr<std::mutex> io_mutex,
                     const std::string& data) {
    int sent = 0, total = data.size();
    while (sent < total) {
        int w;
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            w = libssh2_channel_write(ch, data.c_str() + sent, total - sent);
        }
        if (w == LIBSSH2_ERROR_EAGAIN) { platform::sleep_ms(1); continue; }
        if (w < 0) break;
        sent += w;
    }
}

static std::string ch_read_until(LIBSSH2_CHANNEL* ch,
                                 std::shared_ptr<std::mutex> io_mutex,
                                 int sock,
                                 const std::string& stop,
                                 int timeout_ms) {
    std::string result;
    char buf[16384];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int n;
        {
            std::lock_guard<std::mutex> lock(*io_mutex);
            n = libssh2_channel_read(ch, buf, sizeof(buf));
        }
        if (n > 0) {
            result.append(buf, n);
            if (!stop.empty() && result.find(stop) != std::string::npos) break;
        } else {
            platform::poll_socket(sock, POLLIN, 50);
        }
    }
    return result;
}

static std::string decode_octal(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '3') {
            int val = (s[i+1] - '0') * 64 +
                      (s[i+2] - '0') * 8 +
                      (s[i+3] - '0');
            out += static_cast<char>(val);
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

struct Shell {
    int id;
    int win_idx;
};

static std::vector<Shell> shells;
static int active = -1;  // index into shells
static int next_id = 1;
static int next_win = 1; // window 0 = master

static void create_shell(LIBSSH2_CHANNEL* ch, std::shared_ptr<std::mutex> io_mutex, int sock) {
    int id = next_id++;
    int widx = next_win++;
    // Create window and set $INDEX in it
    ch_write(ch, io_mutex, "new-window -t tccp-exp\n");
    ch_read_until(ch, io_mutex, sock, "", 300);
    ch_write(ch, io_mutex,
        fmt::format("send-keys -t tccp-exp:{} 'export INDEX={}' Enter\n", widx, id));
    shells.push_back({id, widx});
    active = static_cast<int>(shells.size()) - 1;
}

static void switch_to(LIBSSH2_CHANNEL* ch, std::shared_ptr<std::mutex> io_mutex) {
    if (active < 0 || active >= static_cast<int>(shells.size())) return;
    std::string cls = "\033[2J\033[H";
    ::write(STDOUT_FILENO, cls.data(), cls.size());
    ch_write(ch, io_mutex,
        fmt::format("capture-pane -t tccp-exp:{} -p -e\n", shells[active].win_idx));
}

int main() {
    auto load_result = Config::load_global();
    if (load_result.is_err()) { fmt::print("ERROR: {}\n", load_result.error); return 1; }
    Config config = load_result.value;

    auto& creds = CredentialManager::instance();
    auto user_result = creds.get("user");
    auto pass_result = creds.get("password");
    if (user_result.is_err() || pass_result.is_err()) { fmt::print("ERROR: No creds\n"); return 1; }

    SessionTarget target;
    target.host = config.dtn().host;
    target.user = user_result.value;
    target.password = pass_result.value;
    target.timeout = 30;
    target.ssh_key_path = config.dtn().ssh_key_path;
    target.use_duo = true;
    target.auto_duo = config.duo_auto().has_value();

    SessionManager session(target);
    fmt::print("Connecting...\n");
    auto conn_result = session.establish([](const std::string& msg) {
        fmt::print("  {}\n", msg);
    });
    if (conn_result.failed()) { fmt::print("ERROR: {}\n", conn_result.stderr_data); return 1; }

    auto io_mutex = session.io_mutex();
    int sock = session.get_socket();
    LIBSSH2_CHANNEL* ch = session.get_channel();

    {
        SSHConnection tmp(ch, session.get_raw_session(), io_mutex, sock,
                          std::make_shared<std::mutex>());
        tmp.run("tmux kill-session -t tccp-exp 2>/dev/null");
    }

    {
        char drain[4096];
        std::lock_guard<std::mutex> lock(*io_mutex);
        while (libssh2_channel_read(ch, drain, sizeof(drain)) > 0) {}
    }

    ch_write(ch, io_mutex, "unset TMUX; tmux -CC new-session -s tccp-exp\n");

    std::string init = ch_read_until(ch, io_mutex, sock, "%output", 5000);
    if (init.find("%window-add") == std::string::npos &&
        init.find("\033P1000p") == std::string::npos) {
        fmt::print("ERROR: tmux control mode failed\n");
        session.close();
        return 1;
    }
    ch_read_until(ch, io_mutex, sock, "", 1000);

    // Create first user shell
    create_shell(ch, io_mutex, sock);

    struct termios orig, raw_term;
    tcgetattr(STDIN_FILENO, &orig);
    raw_term = orig;
    cfmakeraw(&raw_term);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    fmt::print("\033[2J\033[H");
    fmt::print("^T=new ^W=kill ^N=next ^P=prev ^C=exit\r\n");
    ch_write(ch, io_mutex, "refresh-client\n");

    bool running = true;
    std::string linebuf;
    char rbuf[4096];

    while (running) {
        struct pollfd fds[1];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        poll(fds, 1, 10);

        if (fds[0].revents & POLLIN) {
            int n = ::read(STDIN_FILENO, rbuf, sizeof(rbuf));
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                unsigned char c = rbuf[i];

                if (c == 0x03) { running = false; break; }

                if (c == 0x14) { // Ctrl+T
                    create_shell(ch, io_mutex, sock);
                    switch_to(ch, io_mutex);
                    continue;
                }

                if (c == 0x17) { // Ctrl+W
                    if (shells.size() <= 1) continue;
                    int widx = shells[active].win_idx;
                    shells.erase(shells.begin() + active);
                    ch_write(ch, io_mutex,
                        fmt::format("kill-window -t tccp-exp:{}\n", widx));
                    if (active >= static_cast<int>(shells.size()))
                        active = static_cast<int>(shells.size()) - 1;
                    switch_to(ch, io_mutex);
                    continue;
                }

                if (c == 0x0E) { // Ctrl+N
                    if (shells.size() <= 1) continue;
                    active = (active + 1) % static_cast<int>(shells.size());
                    switch_to(ch, io_mutex);
                    continue;
                }

                if (c == 0x10) { // Ctrl+P
                    if (shells.size() <= 1) continue;
                    active = (active - 1 + static_cast<int>(shells.size()))
                             % static_cast<int>(shells.size());
                    switch_to(ch, io_mutex);
                    continue;
                }

                if (active < 0 || active >= static_cast<int>(shells.size())) continue;
                int widx = shells[active].win_idx;
                std::string cmd;

                if (c == 0x0D || c == 0x0A)
                    cmd = fmt::format("send-keys -t tccp-exp:{} Enter\n", widx);
                else if (c == 0x7F || c == 0x08)
                    cmd = fmt::format("send-keys -t tccp-exp:{} BSpace\n", widx);
                else if (c == 0x09)
                    cmd = fmt::format("send-keys -t tccp-exp:{} Tab\n", widx);
                else if (c == 0x1B)
                    cmd = fmt::format("send-keys -t tccp-exp:{} Escape\n", widx);
                else if (c == 0x04)
                    cmd = fmt::format("send-keys -t tccp-exp:{} -H 04\n", widx);
                else if (c >= 0x20) {
                    if (c == '\'')
                        cmd = fmt::format("send-keys -t tccp-exp:{} -l \"'\"\n", widx);
                    else
                        cmd = fmt::format("send-keys -t tccp-exp:{} -l '{}'\n", widx,
                                          std::string(1, static_cast<char>(c)));
                } else
                    cmd = fmt::format("send-keys -t tccp-exp:{} -H {:02X}\n", widx, c);

                if (!cmd.empty())
                    ch_write(ch, io_mutex, cmd);
            }
        }

        for (;;) {
            int n;
            {
                std::lock_guard<std::mutex> lock(*io_mutex);
                n = libssh2_channel_read(ch, rbuf, sizeof(rbuf));
            }
            if (n <= 0) break;

            linebuf.append(rbuf, n);
            size_t pos = 0;
            while (pos < linebuf.size()) {
                auto nl = linebuf.find('\n', pos);
                if (nl == std::string::npos) break;

                std::string line = linebuf.substr(pos, nl - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pos = nl + 1;

                if (line.size() > 8 && line.substr(0, 8) == "%output ") {
                    auto space1 = line.find(' ', 8);
                    if (space1 != std::string::npos) {
                        std::string decoded = decode_octal(line.substr(space1 + 1));
                        ::write(STDOUT_FILENO, decoded.data(), decoded.size());
                    }
                } else if (!line.empty() && line[0] != '%') {
                    std::string decoded = decode_octal(line);
                    decoded += "\n";
                    ::write(STDOUT_FILENO, decoded.data(), decoded.size());
                }
            }
            if (pos > 0) linebuf = linebuf.substr(pos);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    fmt::print("\n");

    ch_write(ch, io_mutex, "\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        char drain[4096];
        std::lock_guard<std::mutex> lock(*io_mutex);
        while (libssh2_channel_read(ch, drain, sizeof(drain)) > 0) {}
    }
    SSHConnection cleanup(ch, session.get_raw_session(), io_mutex, sock,
                          std::make_shared<std::mutex>());
    cleanup.run("tmux kill-session -t tccp-exp 2>/dev/null");

    fmt::print("Done.\n");
    session.close();
    return 0;
}
