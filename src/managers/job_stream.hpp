#pragma once

#include <string>
#include <atomic>

// Lightweight tracker for a background watcher process on the DTN.
//
// The watcher runs as a backgrounded shell process on the DTN (launched via
// the primary shell channel which has Kerberos). It SSH-hops to the compute
// node, monitors the dtach socket, and touches a marker file when the job
// finishes. No exec channels (which trigger Duo), no background threads.
//
// Completion is detected by checking the marker file via a single batched
// `ls` command in JobManager::check_stream_completions().
class JobStream {
public:
    JobStream(const std::string& job_id,
              const std::string& marker_path);

    const std::string& job_id() const { return job_id_; }
    const std::string& marker_path() const { return marker_path_; }
    bool is_done() const { return done_.load(); }
    void mark_done() { done_.store(true); }

private:
    std::string job_id_;
    std::string marker_path_;
    std::atomic<bool> done_{false};
};
