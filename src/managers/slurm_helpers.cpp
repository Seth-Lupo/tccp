#include "slurm_helpers.hpp"
#include <core/constants.hpp>
#include <fmt/format.h>
#include <sstream>
#include <thread>
#include <chrono>

SlurmJobInfo query_slurm_state(SSHConnection& login, const std::string& slurm_id) {
    std::string cmd = fmt::format("squeue -j {} -h -o \"%T %N\"", slurm_id);

    SlurmJobInfo info;
    for (int attempt = 0; attempt <= SSH_CMD_MAX_RETRIES; attempt++) {
        auto qr = login.run(cmd);
        std::istringstream iss(qr.stdout_data);
        iss >> info.state >> info.node;

        if (!info.state.empty()) return info;

        if (attempt < SSH_CMD_MAX_RETRIES) {
            std::this_thread::sleep_for(std::chrono::seconds(SSH_RETRY_DELAY_SECS));
        }
    }
    return info;
}

std::map<std::string, SlurmJobInfo> query_slurm_batch(SSHConnection& login,
                                                       const std::vector<std::string>& slurm_ids) {
    std::map<std::string, SlurmJobInfo> results;
    if (slurm_ids.empty()) return results;

    // Build comma-separated ID list
    std::string id_list;
    for (const auto& id : slurm_ids) {
        if (!id_list.empty()) id_list += ",";
        id_list += id;
    }

    std::string cmd = fmt::format("squeue -j {} -h -o \"%i %T %N\"", id_list);
    auto qr = login.run(cmd);

    // Parse "SLURM_ID STATE NODE\n..." lines
    std::istringstream iss(qr.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        std::istringstream lss(line);
        std::string id, state, node;
        lss >> id >> state >> node;
        if (!id.empty() && !state.empty()) {
            results[id] = {state, node};
        }
    }

    // IDs not in output are gone (COMPLETED/FAILED/CANCELLED — squeue only shows active)
    // Leave them absent from the map — caller checks .count()

    return results;
}
