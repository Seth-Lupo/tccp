#include "terminal.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#  include <poll.h>
#  include <signal.h>
#endif

namespace platform {

// ── Terminal dimensions ──────────────────────────────────────

int term_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

int term_height() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 24;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
#endif
}

// ── RawModeGuard ─────────────────────────────────────────────

#ifdef _WIN32

RawModeGuard::RawModeGuard(Mode mode) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(h, &old_mode_);
    DWORD new_mode = old_mode_;
    new_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    if (mode == kFullRaw) {
        new_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    }
    SetConsoleMode(h, new_mode);
}

RawModeGuard::~RawModeGuard() {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), old_mode_);
}

#else // Unix

struct RawModeGuard::Impl {
    struct termios old_term;
};

RawModeGuard::RawModeGuard(Mode mode) : impl_(new Impl) {
    tcgetattr(STDIN_FILENO, &impl_->old_term);
    struct termios raw = impl_->old_term;
    if (mode == kFullRaw) {
        cfmakeraw(&raw);
    } else {
        // kNoEcho: disable canonical mode, echo, and signals
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

RawModeGuard::~RawModeGuard() {
    if (impl_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &impl_->old_term);
        delete impl_;
    }
}

#endif

// ── poll_stdin ───────────────────────────────────────────────

bool poll_stdin(int timeout_ms) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD result = WaitForSingleObject(h, timeout_ms);
    if (result == WAIT_OBJECT_0) {
        // Check that there's actual input (not just a window event)
        INPUT_RECORD rec;
        DWORD count;
        while (PeekConsoleInputW(h, &rec, 1, &count) && count > 0) {
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
                return true;
            // Consume non-key events
            ReadConsoleInputW(h, &rec, 1, &count);
        }
        return false;
    }
    return false;
#else
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    return poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
#endif
}

// ── flush_stdin ──────────────────────────────────────────────

void flush_stdin() {
#ifdef _WIN32
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
#else
    tcflush(STDIN_FILENO, TCIFLUSH);
#endif
}

// ── Resize callback ──────────────────────────────────────────

#ifdef _WIN32

// On Windows, console resize events come through ReadConsoleInput.
// For now, provide stub implementations — the job viewer's poll loop
// can check dimensions periodically.
void on_terminal_resize(ResizeCallback) {}
void remove_terminal_resize() {}

#else

static volatile sig_atomic_t g_resize_flag = 0;
static ResizeCallback g_resize_cb;
static struct sigaction g_old_sa;

static void sigwinch_handler(int) {
    g_resize_flag = 1;
}

void on_terminal_resize(ResizeCallback cb) {
    g_resize_cb = std::move(cb);
    g_resize_flag = 0;

    struct sigaction sa;
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, &g_old_sa);
}

void remove_terminal_resize() {
    sigaction(SIGWINCH, &g_old_sa, nullptr);
    g_resize_cb = nullptr;
}

#endif

} // namespace platform
