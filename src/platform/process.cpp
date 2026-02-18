#include "process.hpp"
#include "platform.hpp"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <cstring>
#endif

#include <sstream>

namespace platform {

// ── ProcessHandle ────────────────────────────────────────────

ProcessHandle::ProcessHandle() = default;

ProcessHandle::~ProcessHandle() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (thread_ != INVALID_HANDLE_VALUE) CloseHandle(thread_);
#endif
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept {
#ifdef _WIN32
    handle_ = other.handle_;
    thread_ = other.thread_;
    other.handle_ = INVALID_HANDLE_VALUE;
    other.thread_ = INVALID_HANDLE_VALUE;
#else
    pid_ = other.pid_;
    other.pid_ = -1;
#endif
}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (thread_ != INVALID_HANDLE_VALUE) CloseHandle(thread_);
        handle_ = other.handle_;
        thread_ = other.thread_;
        other.handle_ = INVALID_HANDLE_VALUE;
        other.thread_ = INVALID_HANDLE_VALUE;
#else
        pid_ = other.pid_;
        other.pid_ = -1;
#endif
    }
    return *this;
}

bool ProcessHandle::valid() const {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return pid_ > 0;
#endif
}

bool ProcessHandle::running() const {
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    DWORD code;
    if (GetExitCodeProcess(handle_, &code))
        return code == STILL_ACTIVE;
    return false;
#else
    if (pid_ <= 0) return false;
    int status;
    pid_t ret = waitpid(pid_, &status, WNOHANG);
    return ret == 0;  // 0 means still running
#endif
}

int ProcessHandle::wait(int timeout_ms) {
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return -1;
    DWORD ms = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    WaitForSingleObject(handle_, ms);
    DWORD code = 1;
    GetExitCodeProcess(handle_, &code);
    return static_cast<int>(code);
#else
    if (pid_ <= 0) return -1;
    if (timeout_ms < 0) {
        int status;
        waitpid(pid_, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    // Poll with timeout
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int status;
        pid_t ret = waitpid(pid_, &status, WNOHANG);
        if (ret == pid_) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        sleep_ms(100);
        elapsed += 100;
    }
    return -1;  // timed out
#endif
}

void ProcessHandle::terminate() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
        TerminateProcess(handle_, 1);
        WaitForSingleObject(handle_, 2000);
    }
#else
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    // Wait up to 2s for graceful exit
    for (int i = 0; i < 20; i++) {
        int status;
        if (waitpid(pid_, &status, WNOHANG) == pid_) return;
        sleep_ms(100);
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
#endif
}

// ── spawn ────────────────────────────────────────────────────

#ifdef _WIN32

ProcessHandle spawn(const std::string& program,
                    const std::vector<std::string>& args,
                    const std::string& stderr_log) {
    ProcessHandle handle;

    // Build command line
    std::ostringstream cmdline;
    cmdline << "\"" << program << "\"";
    for (const auto& arg : args) {
        cmdline << " \"" << arg << "\"";
    }
    std::string cmd_str = cmdline.str();

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Redirect stderr if requested
    HANDLE hStderr = INVALID_HANDLE_VALUE;
    if (!stderr_log.empty()) {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        hStderr = CreateFileA(stderr_log.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hStderr != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdError = hStderr;
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }

    if (CreateProcessA(nullptr, cmd_str.data(), nullptr, nullptr, TRUE,
                       0, nullptr, nullptr, &si, &pi)) {
        handle.handle_ = pi.hProcess;
        handle.thread_ = pi.hThread;
    }

    if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
    return handle;
}

#else // Unix

ProcessHandle spawn(const std::string& program,
                    const std::vector<std::string>& args,
                    const std::string& stderr_log) {
    ProcessHandle handle;

    pid_t pid = fork();
    if (pid < 0) return handle;  // fork failed

    if (pid == 0) {
        // Child process
        close(STDIN_FILENO);

        if (!stderr_log.empty()) {
            int fd = open(stderr_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        // Build argv array
        std::vector<const char*> argv;
        argv.push_back(program.c_str());
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(program.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);  // exec failed
    }

    // Parent
    handle.pid_ = pid;
    return handle;
}

#endif

} // namespace platform
