#include "job_manager.hpp"
#include "job_log.hpp"
#include <fmt/format.h>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

Result<void> JobManager::return_output(const std::string& job_id, StatusCallback cb) {
    std::string remote_output = job_output_dir(job_id) + "/output";

    auto ls_result = dtn_.run("find " + remote_output + " -type f 2>/dev/null");
    if (ls_result.stdout_data.empty()) {
        // Try direct output dir
        remote_output = job_output_dir(job_id);
        ls_result = dtn_.run("find " + remote_output + " -type f 2>/dev/null");
    }

    if (ls_result.stdout_data.empty()) {
        if (cb) cb("No output files found.");
        return Result<void>::Ok();
    }

    std::vector<std::string> remote_files;
    std::istringstream iss(ls_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty()) {
            remote_files.push_back(line);
        }
    }

    if (remote_files.empty()) {
        if (cb) cb("No output files found.");
        return Result<void>::Ok();
    }

    fs::path local_base = config_.project_dir() / "tccp-output" / job_id;
    fs::create_directories(local_base);

    int count = 0;
    for (const auto& remote_file : remote_files) {
        std::string rel = remote_file;
        if (rel.find(remote_output) == 0) {
            rel = rel.substr(remote_output.length());
            if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        }
        if (rel.empty()) continue;

        fs::path local_file = local_base / rel;
        dtn_.download(remote_file, local_file);
        count++;
    }

    if (cb) cb(fmt::format("Downloaded {} files to tccp-output/{}/", count, job_id));

    // Delete remote output after successful download
    dtn_.run("rm -rf " + job_output_dir(job_id));

    // Mark output as returned in tracked job and persistent state
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& tj : tracked_) {
            if (tj.job_id == job_id) {
                tj.output_returned = true;
                break;
            }
        }
    }
    for (auto& js : allocs_.state().jobs) {
        if (js.job_id == job_id) {
            js.output_returned = true;
            break;
        }
    }
    allocs_.persist();

    return Result<void>::Ok();
}

void JobManager::try_return_output(TrackedJob& tj) {
    if (tj.output_returned) return;

    std::string remote_output = job_output_dir(tj.job_id);

    // Check if there are any output files
    auto ls_result = dtn_.run("find " + remote_output + " -type f 2>/dev/null");
    if (ls_result.stdout_data.empty()) {
        // No output files — mark as returned (nothing to do)
        tj.output_returned = true;
        for (auto& js : allocs_.state().jobs) {
            if (js.job_id == tj.job_id) {
                js.output_returned = true;
                break;
            }
        }
        allocs_.persist();
        return;
    }

    // Parse remote files
    std::vector<std::string> remote_files;
    std::istringstream iss(ls_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty()) remote_files.push_back(line);
    }

    if (remote_files.empty()) {
        tj.output_returned = true;
        for (auto& js : allocs_.state().jobs) {
            if (js.job_id == tj.job_id) {
                js.output_returned = true;
                break;
            }
        }
        allocs_.persist();
        return;
    }

    // Download all files
    fs::path local_base = config_.project_dir() / "tccp-output" / tj.job_id;
    fs::create_directories(local_base);

    bool all_ok = true;
    for (const auto& remote_file : remote_files) {
        std::string rel = remote_file;
        if (rel.find(remote_output) == 0) {
            rel = rel.substr(remote_output.length());
            if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        }
        if (rel.empty()) continue;

        fs::path local_file = local_base / rel;
        auto dl = dtn_.download(remote_file, local_file);
        if (dl.exit_code != 0) {
            all_ok = false;
            break;
        }
    }

    // Only delete remote output if download fully succeeded
    if (all_ok) {
        dtn_.run("rm -rf " + remote_output);
        tj.output_returned = true;
        for (auto& js : allocs_.state().jobs) {
            if (js.job_id == tj.job_id) {
                js.output_returned = true;
                break;
            }
        }
        allocs_.persist();
        tccp_log(fmt::format("Auto-returned output for job {}", tj.job_id));
    } else {
        tccp_log(fmt::format("Auto-return failed for job {}, remote output preserved", tj.job_id));
    }
}
