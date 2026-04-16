#include "ssh.hpp"
#include "debug.hpp"
#include <fmt/format.h>
#include <cstring>
#include <fstream>
#include <chrono>
#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif

// Inner hop SSH options for compute nodes
static const std::string SSH_OPTS =
    "-o StrictHostKeyChecking=no -o BatchMode=yes -o LogLevel=ERROR";

// ── escape_for_ssh ────────────────────────────────────────

std::string escape_for_ssh(const std::string& cmd) {
    std::string escaped;
    escaped.reserve(cmd.size() + 10);
    escaped += '\'';
    for (char c : cmd) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += '\'';
    return escaped;
}

#ifdef _WIN32
// ── Windows stubs (not supported) ────────────────────────

SSH::SSH(std::string host, std::string login, std::string user, std::string password)
    : host_(std::move(host)), login_(std::move(login)), user_(std::move(user)), password_(std::move(password)) {}

std::vector<std::string> SSH::base_args(bool) const { return {}; }
Result<void> SSH::connect() { return Result<void>::Err("SSH not supported on Windows"); }
void SSH::disconnect() {}
SSHResult SSH::run(const std::string&, int) { return {-1, "", "not supported on Windows"}; }
SSHResult SSH::run_login(const std::string&, int) { return {-1, "", "not supported on Windows"}; }
SSHResult SSH::run_compute(const std::string&, const std::string&, int) { return {-1, "", "not supported on Windows"}; }
Result<void> SSH::tar_push(const std::string&, const fs::path&, const std::vector<std::string>&, const std::string&) { return Result<void>::Err("not supported on Windows"); }
Result<void> SSH::tar_pull(const std::string&, const fs::path&) { return Result<void>::Err("not supported on Windows"); }
int SSH::interactive(const std::string&, const std::string&, const std::vector<int>&) { return -1; }
int SSH::login_shell() { return -1; }
SSHResult SSH::exec_capture(const std::vector<std::string>&, int) { return {-1, "", "not supported on Windows"}; }
int SSH::exec_passthrough(const std::vector<std::string>&) { return -1; }

#else
// ── SSH class ─────────────────────────────────────────────

SSH::SSH(std::string host, std::string login, std::string user, std::string password)
    : host_(std::move(host)), login_(std::move(login)), user_(std::move(user)), password_(std::move(password)) {
    ctl_path_ = home_dir() / ".tccp" / fmt::format("ssh-ctl-{}@{}", user_, host_);
}

std::vector<std::string> SSH::base_args(bool tty) const {
    std::vector<std::string> args = {
        "ssh",
        "-o", "ControlMaster=auto",
        "-o", fmt::format("ControlPath={}", ctl_path_.string()),
        "-o", "ControlPersist=yes",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
    };
    if (tty) {
        args.push_back("-t");
    } else {
        args.push_back("-T");
    }
    args.push_back(fmt::format("{}@{}", user_, host_));
    return args;
}

// ── Connect: establish ControlMaster ──────────────────────

Result<void> SSH::connect() {
    fs::create_directories(ctl_path_.parent_path());

    // Check if master already exists
    auto check = exec_capture({
        "ssh", "-O", "check",
        "-o", fmt::format("ControlPath={}", ctl_path_.string()),
        fmt::format("{}@{}", user_, host_)
    }, 5);
    if (check.exit_code == 0) {
        return Result<void>::Ok();
    }

    // Write password to temp file, askpass script reads it
    fs::path pw_file = ctl_path_.parent_path() / ".pw";
    fs::path askpass = ctl_path_.parent_path() / "askpass.sh";
    {
        std::ofstream f(pw_file);
        f << password_;
        fs::permissions(pw_file, fs::perms::owner_read | fs::perms::owner_write);
    }
    {
        std::ofstream f(askpass);
        f << "#!/bin/sh\ncase \"$1\" in\n*assword*) cat " << pw_file.string()
          << " ;;\n*) echo 1 ;;\nesac\n";
        fs::permissions(askpass, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
    }

    // Establish master with SSH_ASKPASS (no interactive prompt)
    pid_t pid = fork();
    if (pid < 0) {
        fs::remove(pw_file); fs::remove(askpass);
        return Result<void>::Err("fork() failed");
    }
    if (pid == 0) {
        setenv("SSH_ASKPASS", askpass.c_str(), 1);
        setenv("SSH_ASKPASS_REQUIRE", "force", 1);
        // Detach from terminal so SSH uses SSH_ASKPASS
        setsid();
        execlp("ssh", "ssh",
            "-fN",
            "-o", "ControlMaster=yes",
            "-o", fmt::format("ControlPath={}", ctl_path_.string()).c_str(),
            "-o", "ControlPersist=yes",
            "-o", "ServerAliveInterval=30",
            "-o", "ServerAliveCountMax=3",
            "-o", "PreferredAuthentications=keyboard-interactive,password",
            fmt::format("{}@{}", user_, host_).c_str(),
            nullptr);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    fs::remove(pw_file);
    fs::remove(askpass);

    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0) {
        return Result<void>::Err("SSH connection failed");
    }
    return Result<void>::Ok();
}

void SSH::disconnect() {
    exec_capture({
        "ssh", "-O", "exit",
        "-o", fmt::format("ControlPath={}", ctl_path_.string()),
        fmt::format("{}@{}", user_, host_)
    }, 5);
}

// ── Run command on DTN ────────────────────────────────────

SSHResult SSH::run(const std::string& cmd, int timeout) {
    auto args = base_args(false);
    args.push_back(cmd);
    return exec_capture(args, timeout);
}

// ── Run command on login node (via DTN hop) ───────────────

SSHResult SSH::run_login(const std::string& cmd, int timeout) {
    std::string inner = fmt::format("ssh {} {} {} </dev/null",
                                    SSH_OPTS, login_, escape_for_ssh(cmd));
    auto args = base_args(false);
    args.push_back(inner);
    return exec_capture(args, timeout);
}

// ── Run command on compute node (via DTN hop) ─────────────

SSHResult SSH::run_compute(const std::string& node, const std::string& cmd, int timeout) {
    std::string inner = fmt::format("ssh {} {} {} </dev/null",
                                    SSH_OPTS, node, escape_for_ssh(cmd));
    auto args = base_args(false);
    args.push_back(inner);
    return exec_capture(args, timeout);
}

// ── Tar push (local → compute node) ──────────────────────

Result<void> SSH::tar_push(const std::string& node, const fs::path& base_dir,
                           const std::vector<std::string>& files, const std::string& remote_dir) {
    if (files.empty()) return Result<void>::Ok();

    // Build tar file list
    std::string file_list;
    for (const auto& f : files) {
        file_list += escape_for_ssh(f) + " ";
    }

    // Pipeline: tar cf - files | ssh DTN 'exec ssh node "mkdir -p dir && cd dir && tar xf -"'
    std::string inner_cmd = fmt::format("mkdir -p {} && cd {} && tar xf -", remote_dir, remote_dir);
    std::string ssh_cmd = fmt::format("exec ssh {} {} {}",
                                      SSH_OPTS, node, escape_for_ssh(inner_cmd));

    // Build the full pipeline command
    std::string pipeline = fmt::format(
        "tar cf - -C {} {} | ssh -T"
        " -o ControlMaster=auto"
        " -o ControlPath={}"
        " -o ControlPersist=yes"
        " {}@{} {}",
        base_dir.string(), file_list,
        ctl_path_.string(),
        user_, host_, escape_for_ssh(ssh_cmd));

    // Run via shell to handle pipe
    auto result = exec_capture({"sh", "-c", pipeline}, 600);
    if (result.exit_code != 0) {
        return Result<void>::Err(fmt::format("tar push failed: {}", result.err));
    }
    return Result<void>::Ok();
}

// ── Tar pull (DTN → local) ───────────────────────────────

Result<void> SSH::tar_pull(const std::string& remote_dir, const fs::path& local_dir) {
    fs::create_directories(local_dir);

    std::string pipeline = fmt::format(
        "ssh -T"
        " -o ControlMaster=auto"
        " -o ControlPath={}"
        " -o ControlPersist=yes"
        " {}@{} 'cd {} && tar cf - .' | tar xf - -C {}",
        ctl_path_.string(),
        user_, host_, remote_dir, local_dir.string());

    auto result = exec_capture({"sh", "-c", pipeline}, 600);
    if (result.exit_code != 0) {
        return Result<void>::Err(fmt::format("tar pull failed: {}", result.err));
    }
    return Result<void>::Ok();
}

// ── Interactive SSH (inherits terminal) ───────────────────

int SSH::interactive(const std::string& node, const std::string& cmd,
                     const std::vector<int>& ports) {
    auto args = base_args(true);

    // Add port forwarding for outer hop
    for (int port : ports) {
        args.insert(args.end() - 1, "-L");
        args.insert(args.end() - 1, fmt::format("{}:localhost:{}", port, port));
    }

    // Build inner ssh command with port forwarding
    std::string inner = "ssh -t";
    for (int port : ports) {
        inner += fmt::format(" -L {}:localhost:{}", port, port);
    }
    inner += fmt::format(" {} {} {}", SSH_OPTS, node, escape_for_ssh(cmd));
    args.push_back(inner);

    return exec_passthrough(args);
}

// ── Interactive login shell (DTN → login node) ───────────

int SSH::login_shell() {
    auto args = base_args(true);
    // Inner: interactive ssh from DTN to login node with TTY + login shell.
    std::string inner = fmt::format(
        "exec ssh -t {} {} 'exec $SHELL -l'", SSH_OPTS, login_);
    args.push_back(inner);
    return exec_passthrough(args);
}

// ── Process execution ─────────────────────────────────────

SSHResult SSH::exec_capture(const std::vector<std::string>& args, int timeout) {
    SSHResult result{};

    auto t_start = std::chrono::steady_clock::now();
    if (debug_enabled()) {
        std::string joined;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) joined += ' ';
            joined += args[i];
        }
        debug_log("ssh", fmt::format("→ (timeout={}s) {}", timeout, debug_truncate(joined, 8000)));
    }

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        result.exit_code = -1;
        result.err = "pipe() failed";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.exit_code = -1;
        result.err = "fork() failed";
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Read stdout and stderr
    auto read_fd = [](int fd) -> std::string {
        std::string data;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            data.append(buf, n);
        }
        return data;
    };

    // Simple: read both (could use select for large output, but this works for our use)
    result.out = read_fd(stdout_pipe[0]);
    result.err = read_fd(stderr_pipe[0]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    // Trim trailing whitespace from output
    while (!result.out.empty() && (result.out.back() == '\n' || result.out.back() == '\r'))
        result.out.pop_back();

    int status;
    // Wait with timeout
    if (timeout > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (true) {
            int ret = waitpid(pid, &status, WNOHANG);
            if (ret > 0) break;
            if (ret < 0) {
                result.exit_code = -1;
                result.err = "waitpid() failed";
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(pid, SIGTERM);
                sleep_ms(500);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                result.exit_code = -1;
                result.err = "command timed out";
                return result;
            }
            sleep_ms(50);
        }
    } else {
        waitpid(pid, &status, 0);
    }

    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (debug_enabled()) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start).count();
        debug_log("ssh", fmt::format("← rc={} ({}ms) stdout={} bytes stderr={} bytes",
            result.exit_code, ms, result.out.size(), result.err.size()));
        if (!result.out.empty())
            debug_log("ssh", "  stdout: " + debug_truncate(result.out));
        if (!result.err.empty())
            debug_log("ssh", "  stderr: " + debug_truncate(result.err));
    }
    return result;
}

int SSH::exec_passthrough(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif
