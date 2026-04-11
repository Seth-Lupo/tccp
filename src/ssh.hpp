#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <filesystem>

class SSH {
public:
    SSH(std::string host, std::string login, std::string user, std::string password);

    Result<void> connect();
    void disconnect();

    SSHResult run(const std::string& cmd, int timeout = 300);
    SSHResult run_login(const std::string& cmd, int timeout = 300);
    SSHResult run_compute(const std::string& node, const std::string& cmd, int timeout = 300);

    Result<void> tar_push(const std::string& node, const fs::path& base_dir,
                          const std::vector<std::string>& files, const std::string& remote_dir);
    Result<void> tar_pull(const std::string& remote_dir, const fs::path& local_dir);

    int interactive(const std::string& node, const std::string& cmd,
                    const std::vector<int>& ports = {});

private:
    std::string host_, login_, user_, password_;
    fs::path ctl_path_;

    std::vector<std::string> base_args(bool tty = false) const;
    SSHResult exec_capture(const std::vector<std::string>& args, int timeout);
    int exec_passthrough(const std::vector<std::string>& args);
};

std::string escape_for_ssh(const std::string& cmd);
