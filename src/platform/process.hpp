#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace platform {

// Opaque handle to a spawned child process.
class ProcessHandle {
public:
    ProcessHandle();
    ~ProcessHandle();

    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    // True if the process handle is valid (was successfully spawned).
    bool valid() const;

    // True if the process is still running.
    bool running() const;

    // Wait for the process to exit. Returns exit code.
    // timeout_ms = -1 means indefinite wait.
    int wait(int timeout_ms = -1);

    // Terminate the process (SIGTERM then SIGKILL on Unix, TerminateProcess on Windows).
    void terminate();

    // Get the raw pid/handle.
#ifdef _WIN32
    HANDLE native_handle() const { return handle_; }
#else
    int native_handle() const { return pid_; }
#endif

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE thread_ = INVALID_HANDLE_VALUE;
#else
    int pid_ = -1;
#endif
    friend ProcessHandle spawn(const std::string& program,
                               const std::vector<std::string>& args,
                               const std::string& stderr_log);
};

// Spawn a child process.
// stderr_log: if non-empty, redirect child's stderr to this file (append mode).
ProcessHandle spawn(const std::string& program,
                    const std::vector<std::string>& args,
                    const std::string& stderr_log = "");

} // namespace platform
