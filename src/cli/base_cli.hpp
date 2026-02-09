#pragma once

#include <string>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <core/config.hpp>
#include <ssh/cluster_connection.hpp>
#include <managers/state_store.hpp>
#include <managers/allocation_manager.hpp>
#include <managers/sync_manager.hpp>
#include <managers/job_manager.hpp>

class BaseCLI {
public:
    BaseCLI();
    virtual ~BaseCLI() = default;

    using CommandHandler = std::function<void(BaseCLI&, const std::string&)>;

    void add_command(const std::string& name,
                    CommandHandler handler,
                    const std::string& help);

    bool require_config();
    bool require_connection();

    void init_managers();
    void clear_managers();

    void execute_command(const std::string& command, const std::string& args = "");
    void print_help() const;

    // Public state
    std::optional<Config> config;
    std::unique_ptr<ClusterConnection> cluster;
    std::unique_ptr<StateStore> state_store;
    std::unique_ptr<AllocationManager> allocs;
    std::unique_ptr<SyncManager> sync;
    std::unique_ptr<JobManager> jobs;

    // Returns the prompt string for readline
    std::string get_prompt_string() const;

protected:
    std::map<std::string, std::pair<CommandHandler, std::string>> commands_;
};
