#pragma once

#include "types.hpp"
#include "ssh.hpp"
#include "sync.hpp"
#include "state.hpp"
#include "config.hpp"

class Session {
public:
    Session(const Config& cfg, SSH& ssh, Sync& sync, StateStore& store);

    Result<void> start(StatusCallback cb);
    int shell();
    Result<int> exec(const std::string& cmd);
    Result<void> sync_files(StatusCallback cb);
    void status();
    Result<void> stop(StatusCallback cb);
    bool active() const;

private:
    const Config& cfg_;
    SSH& ssh_;
    Sync& sync_;
    StateStore& store_;
    SessionState state_;

    Result<std::string> allocate(StatusCallback cb);
    std::string pick_gpu(const std::string& partition, StatusCallback cb);
    Result<std::string> wait_for_node(const std::string& id, StatusCallback cb);
    Result<void> ensure_container(const std::string& node, StatusCallback cb);
    Result<void> ensure_mksquashfs(const std::string& node);
    Result<void> ensure_dtach(StatusCallback cb);
    Result<void> run_init(const std::string& node, const std::string& scratch, StatusCallback cb);
    Result<void> start_dtach(const std::string& node, const std::string& scratch, StatusCallback cb);

    std::string singularity_cmd(const std::string& scratch, const std::string& inner) const;
    std::string build_env_script() const;

    // Path helpers
    std::string scratch_path() const;
    std::string sif_path() const;
    std::string socket_path() const;
    std::string nfs_output() const;
    std::string tccp_home() const;
    std::string dtach_bin() const;
};
