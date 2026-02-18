#include "job_watcher.hpp"
#include "job_log.hpp"
#include <core/utils.hpp>
#include <platform/platform.hpp>
#include <fmt/format.h>

JobWatcher::JobWatcher(const DTNConfig& dtn) {
    std::string username = get_cluster_username();
    dtn_host_ = fmt::format("{}@{}", username, dtn.host);
    key_path_ = (platform::home_dir() / ".ssh" / "id_ed25519").string();
}

JobWatcher::~JobWatcher() {
    stop_all();
}

void JobWatcher::watch(const std::string& job_id,
                       const std::string& compute_node,
                       const std::string& socket_path,
                       Callback on_complete) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (watchers_.count(job_id)) return;

    std::string inner_cmd = fmt::format(
        "exec ssh -o StrictHostKeyChecking=no -o BatchMode=yes {} "
        "\"while test -S {}; do sleep 1; done\"",
        compute_node, socket_path);

    std::string log_file = (platform::temp_dir() /
        fmt::format("tccp_watcher_{}.log", job_id)).string();

    auto proc = std::make_shared<platform::ProcessHandle>(
        platform::spawn("ssh", {
            "-T",
            "-o", "StrictHostKeyChecking=no",
            "-o", "BatchMode=yes",
            "-o", "ServerAliveInterval=30",
            "-o", "ServerAliveCountMax=3",
            "-i", key_path_,
            dtn_host_,
            inner_cmd
        }, log_file));

    if (!proc->valid()) {
        tccp_log(fmt::format("JobWatcher: spawn failed for {}", job_id));
        return;
    }

    auto entry = std::make_unique<Entry>();
    entry->proc = proc;
    auto* raw = entry.get();
    entry->thread = std::thread(&JobWatcher::watcher_thread, this,
                                 job_id, raw, std::move(on_complete));
    watchers_[job_id] = std::move(entry);
    tccp_log(fmt::format("JobWatcher: watching {}", job_id));
}

void JobWatcher::unwatch(const std::string& job_id) {
    std::unique_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watchers_.find(job_id);
        if (it == watchers_.end()) return;
        it->second->canceled.store(true);
        if (it->second->proc->valid())
            it->second->proc->terminate();
        entry = std::move(it->second);
        watchers_.erase(it);
    }
    if (entry->thread.joinable())
        entry->thread.join();
}

void JobWatcher::stop_all() {
    std::map<std::string, std::unique_ptr<Entry>> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(watchers_);
    }
    for (auto& [id, e] : local) {
        e->canceled.store(true);
        if (e->proc->valid())
            e->proc->terminate();
    }
    for (auto& [id, e] : local) {
        if (e->thread.joinable())
            e->thread.join();
    }
}

void JobWatcher::watcher_thread(std::string job_id,
                                 Entry* entry,
                                 Callback on_complete) {
    entry->proc->wait();
    if (entry->canceled.load()) return;

    tccp_log(fmt::format("JobWatcher: {} exited, signaling", job_id));
    if (on_complete) on_complete();
}
