#pragma once

#include <string>
#include <core/types.hpp>
#include <ssh/session.hpp>

class JobView {
public:
    JobView(SessionManager& session, const std::string& compute_node,
            const std::string& username, const std::string& dtach_socket,
            const std::string& job_name, const std::string& slurm_id,
            const std::string& job_id, const std::string& dtn_host,
            const std::string& scratch_path, bool canceled = false);

    // Relay stdin/stdout through DTN → compute node → dtach.
    // Returns exit code (>= 0) if job finished, -1 if detached, -2 if canceled, -3 if view output.
    // skip_remote_replay: clear screen after job start marker (for fresh starts).
    Result<int> attach(bool skip_remote_replay = false);

    void draw_header(bool terminated = false, int exit_code = 0, bool canceled = false);

private:
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
