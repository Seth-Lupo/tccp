#pragma once

#include <functional>

namespace platform {

// Get terminal dimensions.
int term_width();
int term_height();

// RAII guard for raw terminal mode.
// Constructor saves current mode and enters raw mode.
// Destructor restores the saved mode.
struct RawModeGuard {
    enum Mode {
        kFullRaw,   // cfmakeraw equivalent (for interactive relay)
        kNoEcho,    // Canonical off, echo off (for password input / key detection)
    };

    explicit RawModeGuard(Mode mode = kFullRaw);
    ~RawModeGuard();

    RawModeGuard(const RawModeGuard&) = delete;
    RawModeGuard& operator=(const RawModeGuard&) = delete;

private:
#ifdef _WIN32
    unsigned long old_mode_ = 0;
#else
    struct Impl;
    Impl* impl_ = nullptr;
#endif
};

// Poll stdin for input readability with a timeout.
// Returns true if stdin has data to read.
bool poll_stdin(int timeout_ms);

// Flush any pending input from stdin.
void flush_stdin();

// Window-resize callback.
using ResizeCallback = std::function<void()>;
void on_terminal_resize(ResizeCallback cb);
void remove_terminal_resize();

} // namespace platform
