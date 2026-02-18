#include "job_helpers.hpp"
#include "../theme.hpp"
#include <iostream>
#include <fstream>
#include <fmt/format.h>
#include <filesystem>
#include <algorithm>

void do_clean(BaseCLI& cli, const std::string& arg) {
    namespace fs = std::filesystem;

    fs::path output_root = fs::current_path() / "output";
    if (!fs::exists(output_root) || !fs::is_directory(output_root)) {
        std::cout << "No output directory found.\n";
        return;
    }

    int removed = 0;
    std::error_code ec;

    for (const auto& job_entry : fs::directory_iterator(output_root)) {
        if (!job_entry.is_directory()) continue;
        std::string job_name = job_entry.path().filename().string();

        if (!arg.empty() && job_name != arg) continue;

        fs::path latest_link = job_entry.path() / "latest";
        std::string keep_name;
        if (fs::is_symlink(latest_link)) {
            keep_name = fs::read_symlink(latest_link, ec).string();
        }

        for (const auto& run_entry : fs::directory_iterator(job_entry.path())) {
            std::string name = run_entry.path().filename().string();
            if (name == "latest") continue;
            if (name == keep_name) continue;
            fs::remove_all(run_entry.path(), ec);
            removed++;
        }
    }

    // Prune old terminated jobs (keep only latest per job name)
    if (cli.service.job_manager()) {
        cli.service.prune_old_jobs();
    }

    if (removed > 0) {
        std::cout << fmt::format("Cleaned {} old output run(s). Pruned old job state.\n", removed);
    } else {
        std::cout << "Pruned old job state.\n";
    }
}
