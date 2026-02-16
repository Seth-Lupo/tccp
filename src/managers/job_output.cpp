#include "job_manager.hpp"
#include "job_log.hpp"
#include <core/utils.hpp>
#include <fmt/format.h>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

// Local output path: output/{job_name}/{timestamp}/
static fs::path local_output_dir(const fs::path& project_dir, const std::string& job_id) {
    return project_dir / "output" / JobManager::job_name_from_id(job_id)
                                  / JobManager::timestamp_from_id(job_id);
}

// Update output/{job_name}/latest → {timestamp} (relative symlink)
static void update_latest_symlink(const fs::path& project_dir, const std::string& job_id) {
    fs::path job_dir = project_dir / "output" / JobManager::job_name_from_id(job_id);
    fs::create_directories(job_dir);
    fs::path latest_link = job_dir / "latest";
    std::error_code ec;
    fs::remove(latest_link, ec);
    fs::create_directory_symlink(fs::path(JobManager::timestamp_from_id(job_id)), latest_link, ec);
}

void JobManager::mark_output_returned(const std::string& job_id) {
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& tj : tracked_) {
            if (tj.job_id == job_id) { tj.output_returned = true; break; }
        }
    }
    for (auto& js : allocs_.state().jobs) {
        if (js.job_id == job_id) { js.output_returned = true; break; }
    }
    allocs_.persist();
}

int JobManager::download_remote_output(const std::string& job_id,
                                       const std::string& remote_output) {
    // Parse remote file list
    auto ls_result = dtn_.run("find " + remote_output + " -type f 2>/dev/null");
    if (ls_result.stdout_data.empty()) return 0;

    std::vector<std::string> remote_files;
    std::istringstream iss(ls_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        trim(line);
        if (!line.empty()) remote_files.push_back(line);
    }
    if (remote_files.empty()) return 0;

    // Create local output directory and download
    fs::path local_base = local_output_dir(config_.project_dir(), job_id);
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
        auto dl = dtn_.download(remote_file, local_file);
        if (dl.exit_code != 0) return -1;
        count++;
    }

    if (count > 0) {
        update_latest_symlink(config_.project_dir(), job_id);
    }
    return count;
}

Result<void> JobManager::return_output(const std::string& job_id, StatusCallback cb) {
    std::string remote_output = job_output_dir(job_id) + "/output";

    int count = download_remote_output(job_id, remote_output);
    if (count == 0) {
        // Try direct output dir
        remote_output = job_output_dir(job_id);
        count = download_remote_output(job_id, remote_output);
    }

    if (count == 0) {
        if (cb) cb("No output files found.");
        return Result<void>::Ok();
    }
    if (count < 0) {
        return Result<void>::Err("Download failed");
    }

    std::string jname = JobManager::job_name_from_id(job_id);
    std::string tstamp = JobManager::timestamp_from_id(job_id);
    if (cb) cb(fmt::format("Downloaded {} files to output/{}/{}/", count, jname, tstamp));

    dtn_.run("rm -rf " + job_output_dir(job_id));
    mark_output_returned(job_id);

    return Result<void>::Ok();
}

void JobManager::try_return_output(TrackedJob& tj) {
    if (tj.output_returned) return;

    std::string remote_output = job_output_dir(tj.job_id);
    int count = download_remote_output(tj.job_id, remote_output);

    if (count == 0) {
        mark_output_returned(tj.job_id);
        return;
    }

    if (count > 0) {
        dtn_.run("rm -rf " + remote_output);
        mark_output_returned(tj.job_id);
        tccp_log(fmt::format("Auto-returned output for job {}", tj.job_id));
    } else {
        tccp_log(fmt::format("Auto-return failed for job {}, remote output preserved", tj.job_id));
    }
}
