#include "job_helpers.hpp"
#include "../theme.hpp"
#include <iostream>
#include <fstream>
#include <fmt/format.h>
#include <filesystem>
#include <algorithm>

void do_logs(BaseCLI& cli, const std::string& arg) {
    namespace fs = std::filesystem;

    std::string logs_dir;
    {
        const char* home = std::getenv("HOME");
        if (!home) home = "/tmp";
        logs_dir = std::string(home) + "/.tccp/logs";
    }

    if (!fs::exists(logs_dir)) {
        std::cout << "No logs found.\n";
        return;
    }

    if (arg.empty()) {
        // List all logs with summary
        std::vector<std::pair<std::string, fs::path>> entries;
        for (const auto& entry : fs::directory_iterator(logs_dir)) {
            if (entry.path().extension() == ".log") {
                entries.emplace_back(entry.path().stem().string(), entry.path());
            }
        }

        if (entries.empty()) {
            std::cout << "No logs found.\n";
            return;
        }

        std::sort(entries.begin(), entries.end());

        std::cout << "\n";
        for (const auto& [job_id, path] : entries) {
            std::string job_name = job_id;
            auto pos = job_id.find("__");
            if (pos != std::string::npos && pos + 2 < job_id.size()) {
                job_name = job_id.substr(pos + 2);
            }

            std::ifstream f(path);
            std::string first_line, last_line, line;
            if (f) {
                if (std::getline(f, first_line)) {
                    last_line = first_line;
                    while (std::getline(f, line)) {
                        last_line = line;
                    }
                }
            }

            std::string status = "in progress";
            if (last_line.find("Job completed (exit 0)") != std::string::npos) {
                status = "completed";
            } else if (last_line.find("Job completed") != std::string::npos) {
                status = "failed";
            } else if (last_line.find("Job canceled") != std::string::npos) {
                status = "canceled";
            } else if (last_line.find("Init failed") != std::string::npos) {
                status = "init failed";
            }

            std::cout << fmt::format("  {:<16} {:<14} {}\n", job_name, status, first_line);
        }
        std::cout << "\n";
    } else {
        std::string match_path;

        std::string exact = logs_dir + "/" + arg + ".log";
        if (fs::exists(exact)) {
            match_path = exact;
        } else {
            std::string best_id;
            for (const auto& entry : fs::directory_iterator(logs_dir)) {
                if (entry.path().extension() != ".log") continue;
                std::string job_id = entry.path().stem().string();
                auto pos = job_id.find("__");
                if (pos != std::string::npos && pos + 2 < job_id.size()) {
                    std::string name = job_id.substr(pos + 2);
                    if (name == arg && (best_id.empty() || job_id > best_id)) {
                        best_id = job_id;
                        match_path = entry.path().string();
                    }
                }
            }
        }

        if (match_path.empty()) {
            std::cout << theme::error(fmt::format("No log found for '{}'", arg));
            return;
        }

        std::ifstream f(match_path);
        if (!f) {
            std::cout << theme::error("Could not open log file");
            return;
        }
        std::cout << "\n";
        std::string line;
        while (std::getline(f, line)) {
            std::cout << line << "\n";
        }
        std::cout << "\n";
    }
}

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

    if (removed > 0) {
        std::cout << fmt::format("Cleaned {} old output run(s). Latest kept.\n", removed);
    } else {
        std::cout << "Nothing to clean.\n";
    }
}
