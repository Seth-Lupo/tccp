#pragma once

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <core/types.hpp>
#include <platform/process.hpp>

class JobWatcher {
public:
    using Callback = std::function<void()>;

    explicit JobWatcher(const DTNConfig& dtn);
    ~JobWatcher();

    void watch(const std::string& job_id,
               const std::string& compute_node,
               const std::string& socket_path,
               Callback on_complete);
    void unwatch(const std::string& job_id);
    void stop_all();

private:
    std::string dtn_host_;
    std::string key_path_;

    struct Entry {
        std::shared_ptr<platform::ProcessHandle> proc;
        std::thread thread;
        std::atomic<bool> canceled{false};
    };
    std::map<std::string, std::unique_ptr<Entry>> watchers_;
    mutable std::mutex mutex_;

    void watcher_thread(std::string job_id,
                        Entry* entry,
                        Callback on_complete);
};
