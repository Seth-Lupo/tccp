#include "multiplexed_shell.hpp"
#include "session_multiplexer.hpp"
#include "connection_factory.hpp"
#include <platform/platform.hpp>
#include <platform/terminal.hpp>
#include <platform/socket_util.hpp>
#include <algorithm>
#ifdef _WIN32
#  include <io.h>
#  define STDIN_FILENO  0
#  define STDOUT_FILENO 1
#  define read  _read
#  define write _write
#else
#  include <unistd.h>
#  include <poll.h>
#endif

static const std::string BEGIN_MARKER = "__TCCP_BEGIN__";
static const std::string DONE_MARKER = "__TCCP_DONE__";
// BEGIN marker lets feed() skip all echoed text (which can wrap many lines in
// tmux panes) reliably. The '' shell trick prevents PTY echo from matching.
static const std::string SHELL_CMD_PREFIX = "echo __TCCP_BEG''IN__; ";
static const std::string SHELL_CMD_SUFFIX = " ; echo __TCCP_DO''NE__\n";

// ── Lifecycle ──────────────────────────────────────────────────

MultiplexedShell::MultiplexedShell(ConnectionFactory& factory)
    : factory_(factory) {}

MultiplexedShell::~MultiplexedShell() {
    if (started_) stop();
}

bool MultiplexedShell::start(const std::string& command) {
    mux_ = factory_.multiplexer();
    if (!mux_ || !mux_->is_running()) return false;

    channel_id_ = mux_->open_channel();
    if (channel_id_ < 0) return false;

    started_ = true;
    skip_echo_ = true;
    done_ = false;
    buf_.clear();

    // Register output callback
    mux_->set_output_callback(channel_id_,
        [this](const std::string& data) { output_callback(data); });

    // Send command wrapped in BEGIN/DONE markers via send-keys.
    // BEGIN marker lets feed() skip all echoed text reliably.
    std::string cmd = SHELL_CMD_PREFIX + command + SHELL_CMD_SUFFIX;
    mux_->send_input(channel_id_, cmd.c_str(), static_cast<int>(cmd.size()));

    return true;
}

std::string MultiplexedShell::feed(const char* data, int len) {
    if (len <= 0 || done_) return {};

    buf_.append(data, len);

    // Phase 1: skip everything until BEGIN marker.
    // The echoed command can wrap across many lines in the tmux pane,
    // so we can't just skip one line. The BEGIN marker reliably marks
    // where actual command output starts.
    if (skip_echo_) {
        auto bm = buf_.find(BEGIN_MARKER);
        if (bm == std::string::npos) return {};
        // Skip the marker itself and any trailing \r\n
        size_t start = bm + BEGIN_MARKER.size();
        if (start < buf_.size() && buf_[start] == '\r') start++;
        if (start < buf_.size() && buf_[start] == '\n') start++;
        buf_ = buf_.substr(start);
        skip_echo_ = false;
        if (buf_.empty()) return {};
    }

    // Phase 2: check for done marker
    auto dm = buf_.find(DONE_MARKER);
    if (dm != std::string::npos) {
        std::string result = buf_.substr(0, dm);
        buf_.clear();
        done_ = true;
        return result;
    }

    // Hold back bytes that could be the start of the done marker
    size_t hold = 0;
    for (size_t i = 1; i <= (std::min)(buf_.size(), DONE_MARKER.size()); i++) {
        if (buf_.compare(buf_.size() - i, i, DONE_MARKER, 0, i) == 0)
            hold = i;
    }

    if (hold >= buf_.size()) return {};
    std::string result = buf_.substr(0, buf_.size() - hold);
    buf_ = buf_.substr(buf_.size() - hold);
    return result;
}

std::string MultiplexedShell::read_output() {
    std::string result;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!output_queue_.empty()) {
        result += output_queue_.front();
        output_queue_.pop_front();
    }
    return result;
}

bool MultiplexedShell::done() const { return done_; }

void MultiplexedShell::stop() {
    if (!started_) return;

    if (mux_ && channel_id_ >= 0) {
        mux_->clear_output_callback(channel_id_);
        // close_channel kills the tmux pane, which terminates the SSH viewer
        // process. The actual job lives in dtach and survives independently.
        // No Ctrl+C needed — that would kill the remote job on detach/view.
        mux_->close_channel(channel_id_);
    }

    started_ = false;
    channel_id_ = -1;
}

int MultiplexedShell::channel_id() const { return channel_id_; }

SessionMultiplexer& MultiplexedShell::multiplexer() const { return *mux_; }

int MultiplexedShell::socket() const {
    return mux_ ? mux_->raw_socket() : -1;
}

// ── Simple interface ───────────────────────────────────────────

static volatile int g_mux_relay_resize = 0;

void MultiplexedShell::run(const std::string& command) {
    if (!start(command)) return;

    platform::on_terminal_resize([] { g_mux_relay_resize = 1; });
    platform::RawModeGuard raw(platform::RawModeGuard::kFullRaw);

    // Set initial size
    mux_->resize(channel_id_, platform::term_width(), platform::term_height());

    while (!done_) {
        if (g_mux_relay_resize) {
            g_mux_relay_resize = 0;
            mux_->resize(channel_id_, platform::term_width(), platform::term_height());
        }

        // Read output from queue
        std::string output = read_output();
        if (!output.empty()) {
            std::string text = feed(output.data(), static_cast<int>(output.size()));
            if (!text.empty())
                write(STDOUT_FILENO, text.data(), text.size());
            if (done_) break;
        }

        // Check stdin
        bool stdin_ready = false;
#ifdef _WIN32
        stdin_ready = platform::poll_stdin(0);
#else
        struct pollfd fds[1];
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        poll(fds, 1, output.empty() ? 50 : 0);
        stdin_ready = fds[0].revents & POLLIN;
#endif

        if (stdin_ready) {
            char buf[16384];
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            mux_->send_input(channel_id_, buf, n);
        }

        if (!stdin_ready && output.empty())
            platform::sleep_ms(5);
    }

    platform::remove_terminal_resize();
    stop();
}

void MultiplexedShell::output_callback(const std::string& data) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    output_queue_.push_back(data);
}
