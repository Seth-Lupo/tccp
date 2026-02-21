#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <core/types.hpp>

// libssh2 forward declarations
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

// SessionMultiplexer: multiplexes a single SSH shell channel into independent
// virtual channels via tmux -CC (control mode).
//
// Architecture:
//   - One tmux server runs inside the primary shell channel
//   - tmux -CC exposes a structured protocol (%begin/%end/%output)
//   - Each "channel" is a tmux pane/window
//   - A reader thread routes output by pane ID to per-channel queues
//   - run() sends commands with BEGIN/DONE markers via send-keys, waits for result
//   - Interactive sessions get raw output via callbacks
//
// Locking DAG: channel.cmd_mutex → protocol_mutex_ → io_mutex_
// (never acquire in reverse order)

class SessionMultiplexer {
public:
    static constexpr int kMasterChannel = 0;

    SessionMultiplexer(LIBSSH2_CHANNEL* channel, LIBSSH2_SESSION* session,
                       std::shared_ptr<std::mutex> io_mutex, int sock);
    ~SessionMultiplexer();

    // Start tmux -CC on the primary channel.
    SSHResult start();

    // Shut down tmux and stop the reader thread.
    void stop();

    bool is_running() const;

    // ── Channel lifecycle ──────────────────────────────────────

    // Open a new tmux window, returns channel ID. -1 on failure.
    int open_channel();

    // Close and destroy a channel's tmux window.
    void close_channel(int id);

    // ── Programmatic command execution ─────────────────────────

    // Run a command on a channel, wait for BEGIN/DONE markers.
    // Per-channel cmd_mutex serialization (not global).
    SSHResult run(int channel_id, const std::string& cmd, int timeout_secs = 0);

    // ── Interactive I/O ────────────────────────────────────────

    using OutputCallback = std::function<void(const std::string& data)>;

    void send_input(int channel_id, const char* data, int len);
    void send_special_key(int channel_id, const std::string& key);
    void set_output_callback(int channel_id, OutputCallback cb);
    void clear_output_callback(int channel_id);
    void resize(int channel_id, int cols, int rows);

    // ── Raw access ─────────────────────────────────────────────

    LIBSSH2_CHANNEL* raw_channel() const { return channel_; }
    LIBSSH2_SESSION* raw_session() const { return session_; }
    int raw_socket() const { return sock_; }
    std::shared_ptr<std::mutex> io_mutex() const { return io_mutex_; }

private:
    struct ChannelState {
        int id;
        std::string tmux_pane_id;  // e.g. "%0", "%1"
        std::shared_ptr<std::mutex> cmd_mutex;

        // For programmatic run(): accumulated output waiting for markers
        std::string pending_output;
        std::condition_variable output_cv;
        std::mutex output_mutex;
        bool output_ready = false;

        // For interactive: callback receives raw output
        OutputCallback output_cb;
        std::mutex cb_mutex;
    };

    LIBSSH2_CHANNEL* channel_;
    LIBSSH2_SESSION* session_;
    std::shared_ptr<std::mutex> io_mutex_;
    int sock_;

    std::atomic<bool> running_{false};
    std::thread reader_thread_;

    // Protocol mutex: serializes tmux control commands (new-window, send-keys, etc.)
    std::mutex protocol_mutex_;

    // Channel registry
    std::mutex channels_mutex_;
    std::unordered_map<int, std::shared_ptr<ChannelState>> channels_;
    std::unordered_map<std::string, int> pane_to_channel_;  // tmux pane ID → our channel ID
    int next_channel_id_ = 0;

    // Command response queue: reader thread pushes %begin/%end responses,
    // send_tmux_command() waits on the condition variable.
    struct CmdResponse { bool success; std::string output; };
    std::mutex cmd_response_mutex_;
    std::condition_variable cmd_response_cv_;
    std::vector<CmdResponse> cmd_responses_;
    bool cmd_response_ready_ = false;

    // Reader thread: reads channel, parses tmux -C protocol, routes output
    void reader_loop();

    // Send raw text to the tmux control channel (brief io_mutex_ hold)
    bool send_raw(const std::string& text);

    // Send a tmux command and wait for %begin/%end response (via reader thread)
    struct TmuxResponse { bool success; std::string output; };
    TmuxResponse send_tmux_command(const std::string& cmd);

    // Escape printable text for tmux send-keys -l (double-quote encoding)
    static std::string escape_for_send_keys(const std::string& text);

    // Type text into a pane via send-keys commands.
    // Handles newlines (→ Enter), control chars (→ key names), escape sequences
    // (→ arrow/special keys), and printable text (→ send-keys -l).
    // CALLER MUST HOLD protocol_mutex_.
    void type_into_pane(const std::string& pane_id, const char* data, size_t len);

    // Register the master channel with its actual tmux pane ID
    void register_master_channel(const std::string& pane_id);
};
