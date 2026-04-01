#include "job_manager.hpp"
#include "job_log.hpp"
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <ssh/compute_hop.hpp>
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

Result<int> JobManager::download_remote_output(const std::string& job_id,
                                                const std::string& remote_output,
                                                const std::string& compute_node) {
    // List files on compute node via SSH hop
    ComputeHop compute(dtn_, compute_node);
    auto ls_result = compute.run(
        fmt::format("find {} -type f 2>/dev/null", remote_output), 15);

    if (ls_result.exit_code != 0) {
        return Result<int>::Err(fmt::format(
            "Cannot reach {} (exit {}): {}",
            compute_node, ls_result.exit_code, ls_result.stderr_data));
    }

    if (ls_result.stdout_data.empty()) return Result<int>::Ok(0);

    std::vector<std::string> remote_files;
    std::istringstream iss(ls_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        trim(line);
        if (!line.empty()) remote_files.push_back(line);
    }
    if (remote_files.empty()) return Result<int>::Ok(0);

    // Create local output directory and download via SSH hop + base64
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
        fs::create_directories(local_file.parent_path());

        // Download via base64 through SSH hop (compute node → DTN → local)
        auto dl = compute.run(fmt::format("base64 {}", remote_file), 120);
        if (dl.exit_code != 0) {
            return Result<int>::Err(fmt::format(
                "Failed to download {}: exit {} — {}",
                rel, dl.exit_code, dl.stderr_data));
        }
        if (dl.stdout_data.empty()) {
            return Result<int>::Err(fmt::format(
                "Empty response downloading {} (file may be empty or node unreachable)", rel));
        }

        // Decode base64 and write to local file
        auto decoded = base64_decode(dl.stdout_data);
        std::ofstream out(local_file, std::ios::binary);
        if (!out) {
            return Result<int>::Err(fmt::format(
                "Cannot write local file: {}", local_file.string()));
        }
        out.write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        out.close();
        count++;
    }

    if (count > 0) {
        update_latest_symlink(config_.project_dir(), job_id);
    }
    return Result<int>::Ok(count);
}

// Submit a minimal, short-lived allocation pinned to a specific node
// for output recovery. Returns the SLURM job ID, or "" on failure.
std::string JobManager::allocate_recovery(const std::string& compute_node,
                                          const std::string& partition,
                                          StatusCallback cb) {
    // GPU partitions require a GPU resource request even for minimal jobs
    bool is_gpu = partition.find("gpu") != std::string::npos;
    std::string script = fmt::format(
        "#!/bin/bash\n"
        "#SBATCH --job-name=tccp-recover\n"
        "#SBATCH --time=0:05:00\n"
        "#SBATCH --ntasks=1\n"
        "#SBATCH --mem=1G\n"
        "#SBATCH --nodelist={}\n"
        "{}"
        "{}"
        "#SBATCH --output=/dev/null\n"
        "#SBATCH --error=/dev/null\n"
        "\nsleep 300\n",
        compute_node,
        partition.empty() ? "" : fmt::format("#SBATCH --partition={}\n", partition),
        is_gpu ? "#SBATCH --gres=gpu:1\n" : "");

    std::string submit_cmd = "sbatch << 'TCCP_RECOVER_EOF'\n"
                             + script + "\nTCCP_RECOVER_EOF";
    auto result = login_.run(submit_cmd, 30);
    tccp_log(fmt::format("recovery sbatch: exit={} stdout=[{}] stderr=[{}]",
                         result.exit_code, result.stdout_data, result.stderr_data));

    std::string slurm_id;
    auto pos = result.stdout_data.find("Submitted batch job ");
    if (pos != std::string::npos) {
        std::istringstream iss(result.stdout_data.substr(pos + 20));
        iss >> slurm_id;
    }
    return slurm_id;
}

// Wait for a recovery allocation to reach RUNNING state.
// Returns true if running, false on timeout/failure.
bool JobManager::wait_recovery_allocation(const std::string& slurm_id) {
    for (int i = 0; i < 24; i++) {  // 24 * 5s = 2 min max wait
        auto state = query_alloc_state(slurm_id);
        if (state.state == "RUNNING") return true;
        if (state.state.empty() || state.state == "FAILED" ||
            state.state == "CANCELLED" || state.state == "COMPLETED") {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(ALLOC_WAIT_POLL_SECS));
    }
    return false;
}

Result<void> JobManager::return_output(const std::string& job_id, StatusCallback cb) {
    // Find the tracked job
    std::string compute_node;
    std::string scratch_path;
    std::string partition;
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (const auto& tj : tracked_) {
            if (tj.job_id == job_id) {
                compute_node = tj.compute_node;
                scratch_path = tj.scratch_path;
                partition = tj.partition;
                break;
            }
        }
    }

    if (compute_node.empty() || scratch_path.empty()) {
        return Result<void>::Err("Cannot return output: job state missing");
    }

    std::string remote_output = scratch_path + "/output";
    auto dl_result = download_remote_output(job_id, remote_output, compute_node);

    // If direct download failed, try recovery allocation
    if (dl_result.is_err()) {
        if (cb) cb(fmt::format("Node {} unreachable, requesting recovery allocation...", compute_node));

        // Fall back to sinfo if partition wasn't saved
        if (partition.empty()) {
            auto part_result = login_.run(fmt::format(
                "sinfo -n {} -o \"%P\" -h | tr -d '*'", compute_node), 10);
            std::istringstream pss(part_result.stdout_data);
            std::string p;
            std::vector<std::string> candidates;
            while (std::getline(pss, p)) {
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' '))
                    p.pop_back();
                if (!p.empty()) candidates.push_back(p);
            }
            // Try each partition until sbatch succeeds
            std::string recovery_id;
            for (const auto& part : candidates) {
                recovery_id = allocate_recovery(compute_node, part);
                if (!recovery_id.empty()) {
                    partition = part;
                    break;
                }
            }
            if (recovery_id.empty()) {
                return Result<void>::Err(fmt::format(
                    "Could not submit recovery allocation on {}", compute_node));
            }
            if (cb) cb(fmt::format("Recovery allocation submitted ({}), waiting for node (up to 2 min)...", partition));
            if (!wait_recovery_allocation(recovery_id)) {
                login_.run("scancel " + recovery_id);
                return Result<void>::Err("Recovery allocation did not start — node may be unavailable");
            }
            if (cb) cb(fmt::format("Node {} available, downloading...", compute_node));
            dl_result = download_remote_output(job_id, remote_output, compute_node);
            login_.run("scancel " + recovery_id);
        } else {
            // Have saved partition — single attempt
            std::string recovery_id = allocate_recovery(compute_node, partition);
            if (recovery_id.empty()) {
                return Result<void>::Err(fmt::format(
                    "Could not submit recovery allocation on {} ({})", compute_node, partition));
            }
            if (cb) cb(fmt::format("Recovery allocation submitted ({}), waiting for node (up to 2 min)...", partition));
            if (!wait_recovery_allocation(recovery_id)) {
                login_.run("scancel " + recovery_id);
                return Result<void>::Err("Recovery allocation did not start — node may be unavailable");
            }
            if (cb) cb(fmt::format("Node {} available, downloading...", compute_node));
            dl_result = download_remote_output(job_id, remote_output, compute_node);
            login_.run("scancel " + recovery_id);
        }

        if (dl_result.is_err()) {
            return Result<void>::Err(fmt::format(
                "Download failed even with recovery: {}", dl_result.error));
        }
    }

    int count = dl_result.value;
    if (count == 0) {
        if (cb) cb("No output files found.");
        return Result<void>::Ok();
    }

    std::string jname = JobManager::job_name_from_id(job_id);
    std::string tstamp = JobManager::timestamp_from_id(job_id);
    if (cb) {
        std::string file_word = (count == 1) ? "file" : "files";
        cb(fmt::format("Output saved to output/{}/{}/  ({} {})", jname, tstamp, count, file_word));
    }

    // No NFS cleanup needed — output is on scratch, cleaned by scratch lifecycle
    mark_output_returned(job_id);

    return Result<void>::Ok();
}

void JobManager::try_return_output(TrackedJob& tj, bool allow_recovery) {
    if (tj.output_returned) return;

    if (tj.compute_node.empty() || tj.scratch_path.empty()) {
        mark_output_returned(tj.job_id);
        return;
    }

    std::string remote_output = tj.scratch_path + "/output";
    auto dl_result = download_remote_output(tj.job_id, remote_output, tj.compute_node);

    if (dl_result.is_ok()) {
        mark_output_returned(tj.job_id);
        if (dl_result.value > 0) {
            tccp_log(fmt::format("Auto-returned {} output files for job {}", dl_result.value, tj.job_id));
        }
        return;
    }

    if (!allow_recovery) {
        // Quick path (cancel, poll) — don't block on recovery allocation
        mark_output_returned(tj.job_id);
        tccp_log(fmt::format("Output download failed for job {}: {}", tj.job_id, dl_result.error));
        return;
    }

    // Download failed — node likely unreachable (allocation expired).
    // Try to get a short recovery allocation on the same node.
    tccp_log(fmt::format("Output download failed for job {}: {} — attempting recovery on {}",
                         tj.job_id, dl_result.error, tj.compute_node));

    // Find partition from the original allocation
    std::string partition;
    for (const auto& alloc : allocs_.state().allocations) {
        if (alloc.slurm_id == tj.slurm_id) {
            partition = alloc.partition;
            break;
        }
    }

    std::string recovery_id = allocate_recovery(tj.compute_node, partition);
    if (recovery_id.empty()) {
        tccp_log(fmt::format("Recovery allocation failed for job {}, giving up", tj.job_id));
        mark_output_returned(tj.job_id);
        return;
    }

    tccp_log(fmt::format("Recovery allocation {} submitted for node {}", recovery_id, tj.compute_node));

    if (!wait_recovery_allocation(recovery_id)) {
        tccp_log(fmt::format("Recovery allocation {} did not start, giving up", recovery_id));
        login_.run("scancel " + recovery_id);
        mark_output_returned(tj.job_id);
        return;
    }

    // Retry download with the recovery allocation active
    auto retry = download_remote_output(tj.job_id, remote_output, tj.compute_node);
    mark_output_returned(tj.job_id);

    // Release recovery allocation
    login_.run("scancel " + recovery_id);

    if (retry.is_ok() && retry.value > 0) {
        tccp_log(fmt::format("Recovery succeeded: returned {} output files for job {}", retry.value, tj.job_id));
    } else if (retry.is_err()) {
        tccp_log(fmt::format("Recovery retry failed for job {}: {}", tj.job_id, retry.error));
    } else {
        tccp_log(fmt::format("Recovery allocation started but no output found for job {}", tj.job_id));
    }
}
