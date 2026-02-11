#pragma once

#include <string>
#include <core/types.hpp>
#include <ssh/session.hpp>

class JobView {
public:
    JobView(SessionManager& session, const std::string& compute_node,
            const std::string& username, const std::string& dtach_socket,
            const std::string& job_name, const std::string& slurm_id,
            const std::string& output_file);

    // Relay stdin/stdout through DTN → compute node → dtach.
    // Draws a bottom status bar, captures filtered output to file.
    // Returns exit code (>= 0) if job finished, or -1 if user detached.
    // skip_remote_replay: caller already replayed local capture, skip remote tail.
    Result<int> attach(bool skip_remote_replay = false);

    void draw_header(bool terminated = false, int exit_code = 0);

private:
    SessionManager& session_;
    std::string compute_node_;
    std::string username_;
    std::string dtach_socket_;
    std::string job_name_;
    std::string slurm_id_;
    std::string output_file_;
    bool got_first_output_ = false;
};
