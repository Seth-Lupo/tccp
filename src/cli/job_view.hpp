#pragma once

#include <string>
#include <libssh2.h>
#include <core/types.hpp>
#include <ssh/session.hpp>

class JobView {
public:
    static constexpr int kDetached   = -1;
    static constexpr int kCanceled   = -2;
    static constexpr int kViewOutput = -3;

    JobView(SessionManager& session, const std::string& compute_node,
            const std::string& username, const std::string& dtach_socket,
            const std::string& job_name, const std::string& slurm_id,
            const std::string& job_id, const std::string& dtn_host,
            const std::string& scratch_path, bool canceled = false);

    // Relay stdin/stdout through DTN → compute node → dtach.
    // Returns exit code (>= 0) if job finished, or kDetached/kCanceled/kViewOutput.
    // skip_remote_replay: clear screen after job start marker (for fresh starts).
    Result<int> attach(bool skip_remote_replay = false);

    void draw_header(bool terminated = false, int exit_code = 0, bool canceled = false);

private:
    struct RelayState {
        int cols;
        int pty_rows;
        bool skip_remote_replay;
    };

    Result<LIBSSH2_CHANNEL*> open_channel(LIBSSH2_SESSION* ssh, int cols, int pty_rows);
    Result<int> relay_loop(LIBSSH2_CHANNEL* channel, RelayState& state);
    SessionManager& session_;
    std::string compute_node_;
    std::string username_;
    std::string dtach_socket_;
    std::string job_name_;
    std::string slurm_id_;
    std::string job_id_;
    std::string dtn_host_;
    std::string scratch_path_;
    bool got_first_output_ = false;
    bool canceled_ = false;
};
