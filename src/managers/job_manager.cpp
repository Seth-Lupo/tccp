#include "job_manager.hpp"
#include "gitignore.hpp"
#include <core/credentials.hpp>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <set>
#include <filesystem>
#include <ctime>
#include <iomanip>

namespace fs = std::filesystem;

// ── Timestamp helpers ──────────────────────────────────────

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(buf);
}

// ── Debug log ──────────────────────────────────────────────

static const char* TCCP_LOG_PATH = "/tmp/tccp_debug.log";

static void tccp_log(const std::string& msg) {
    std::ofstream out(TCCP_LOG_PATH, std::ios::app);
    if (!out) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char ts[32];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    out << "[" << ts << "] " << msg << "\n";
}

static void tccp_log_ssh(const std::string& label, const std::string& cmd,
                          const SSHResult& r) {
    tccp_log(fmt::format("{} CMD: {}", label, cmd));
    tccp_log(fmt::format("{} exit={} stdout({})={}", label, r.exit_code,
                         r.stdout_data.size(), r.stdout_data.substr(0, 500)));
    if (!r.stderr_data.empty())
        tccp_log(fmt::format("{} stderr={}", label, r.stderr_data.substr(0, 500)));
}

static std::string get_cluster_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "unknown";
}

// ── Constructor ────────────────────────────────────────────

JobManager::JobManager(const Config& config, SSHConnection& dtn, SSHConnection& login,
                       AllocationManager& allocs, SyncManager& sync)
    : config_(config), dtn_(dtn), login_(login),
      allocs_(allocs), sync_(sync),
      username_(get_cluster_username()) {

    // Restore tracked jobs from persistent state
    auto& state = allocs_.state();
    for (const auto& js : state.jobs) {
        TrackedJob tj;
        tj.job_id = js.job_id;
        tj.job_name = js.job_name;
        tj.slurm_id = js.alloc_slurm_id;
        tj.compute_node = js.compute_node;
        tj.completed = js.completed;
        tj.canceled = js.canceled;
        tj.exit_code = js.exit_code;
        tj.output_file = js.output_file;
        tj.scratch_path = js.scratch_path;
        tj.init_complete = js.init_complete;
        tj.init_error = js.init_error;
        tj.submit_time = js.submit_time;
        tj.start_time = js.start_time;
        tj.end_time = js.end_time;
        tracked_.push_back(tj);
    }
}

// ── Path helpers (new directory structure) ──────────────────

std::string JobManager::persistent_base() const {
    return fmt::format("/cluster/home/{}/tccp/projects/{}",
                       username_, config_.project().name);
}

std::string JobManager::job_output_dir(const std::string& job_id) const {
    return persistent_base() + "/output/" + job_id;
}

std::string JobManager::env_dir() const {
    return persistent_base() + "/env";
}

std::string JobManager::container_cache() const {
    return fmt::format("/cluster/home/{}/tccp/container-cache", username_);
}

std::string JobManager::scratch_dir(const std::string& job_id) const {
    return fmt::format("/tmp/{}/{}/{}", username_, config_.project().name, job_id);
}

std::string JobManager::generate_job_id(const std::string& job_name) const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);

    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}-{:02d}-{:02d}-{:03d}__{}",
                       tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                       tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                       static_cast<int>(ms.count()), job_name);
}

// ── Directory setup ────────────────────────────────────────

void JobManager::ensure_dirs(const std::string& job_id, StatusCallback cb) {
    std::string base = persistent_base();
    std::string out = job_output_dir(job_id);
    std::string env = env_dir();
    std::string cc = container_cache();

    std::string cmd = fmt::format(
        "mkdir -p {} {}/default/venv {}/images {}/cache {}/tmp {}",
        base, env, cc, cc, cc, out);
    dtn_.run(cmd);
}

void JobManager::ensure_environment(StatusCallback cb) {
    if (cb) cb("Checking environment...");

    std::string cc = container_cache();
    std::string image = cc + "/images/python_3.11-slim.sif";
    std::string venv = env_dir() + "/default/venv";

    // Ensure singularity/apptainer is available
    dtn_.run("module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true");

    // Pull container image if missing
    std::string check_image = fmt::format("test -f {} && echo EXISTS || echo MISSING", image);
    auto result = dtn_.run(check_image);
    if (result.stdout_data.find("MISSING") != std::string::npos) {
        if (cb) cb("Pulling Python container image (first time only)...");
        std::string pull_cmd = fmt::format(
            "SINGULARITY_CACHEDIR={0}/cache "
            "SINGULARITY_TMPDIR={0}/tmp "
            "singularity pull --dir {0}/images "
            "docker://python:3.11-slim", cc);
        dtn_.run(pull_cmd);
    }

    // Create venv if missing
    std::string check_venv = fmt::format("test -f {}/bin/python && echo EXISTS || echo MISSING", venv);
    result = dtn_.run(check_venv);
    if (result.stdout_data.find("MISSING") != std::string::npos) {
        if (cb) cb("Creating Python virtual environment...");
        std::string base = persistent_base();
        std::string venv_cmd = fmt::format(
            "singularity exec --bind {0}:{0} {1} python -m venv {2}",
            base, image, venv);
        dtn_.run(venv_cmd);
    }

    ensure_dtach(cb);
    if (cb) cb("Environment OK");
}

void JobManager::ensure_dtach(StatusCallback cb) {
    std::string tccp_home = fmt::format("/cluster/home/{}/tccp", username_);
    std::string bin = tccp_home + "/bin/dtach";

    auto check = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_MISSING");
    if (check.stdout_data.find("DTACH_OK") != std::string::npos) {
        return;
    }

    if (cb) cb("Building dtach (first time only)...");

    std::string build_dir = tccp_home + "/dtach-build";
    dtn_.run("mkdir -p " + tccp_home + "/bin");
    dtn_.run("rm -rf " + build_dir);
    dtn_.run("git clone https://github.com/crigler/dtach.git " + build_dir);

    auto build = dtn_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1");
    if (build.exit_code != 0) {
        dtn_.run("cd " + build_dir + " && ./configure && make 2>&1");
    }

    dtn_.run("cp " + build_dir + "/dtach " + bin + " && chmod +x " + bin);
    dtn_.run("rm -rf " + build_dir);

    auto verify = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_FAIL");
    if (verify.stdout_data.find("DTACH_OK") != std::string::npos) {
        if (cb) cb("dtach built successfully");
    } else {
        if (cb) cb("Warning: dtach build may have failed");
    }
}

// ── Launch job on compute node ─────────────────────────────

Result<void> JobManager::launch_on_node(const std::string& job_id,
                                         const std::string& job_name,
                                         const std::string& compute_node,
                                         const std::string& scratch,
                                         StatusCallback cb) {
    const auto& proj = config_.project();
    auto it = proj.jobs.find(job_name);
    std::string script = (it != proj.jobs.end()) ? it->second.script : "main.py";
    std::string args = (it != proj.jobs.end()) ? it->second.args : "";

    std::string cc = container_cache();
    std::string image = cc + "/images/python_3.11-slim.sif";
    std::string venv = env_dir() + "/default/venv";
    std::string out_dir = job_output_dir(job_id);
    std::string sock = "/tmp/tccp_" + job_id + ".sock";
    std::string log = "/tmp/tccp_" + job_id + ".log";

    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";

    if (cb) cb("Launching job on compute node...");

    // Create the run script on the compute node
    std::string run_script = fmt::format(
        "#!/bin/bash\n"
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true\n"
        "export SINGULARITY_CACHEDIR={}/cache\n"
        "export SINGULARITY_TMPDIR={}/tmp\n"
        "cd {}\n"
        "if [ -f requirements.txt ]; then\n"
        "    singularity exec --bind {}:{} --bind {}:{} {} {}/bin/pip install -q -r requirements.txt 2>/dev/null\n"
        "fi\n"
        "printf '\\n__TCCP_JOB_START__\\n'\n"
        "CMD=\"singularity exec --bind {}:{} {} {}/bin/python {} {}\"\n"
        "script -fq -c \"$CMD\" {}\n"
        "EC=$?\n"
        "printf '\\033[J'\n"
        "exit $EC\n",
        cc, cc,
        scratch,
        scratch, scratch, venv, venv, image, venv,
        scratch, scratch, image, venv, script, args,
        log);

    // Write run script via DTN → compute node
    std::string write_cmd = fmt::format(
        "ssh {} {} 'cat > {}/tccp_run.sh << '\"'\"'TCCP_RUN_EOF'\"'\"'\n{}\nTCCP_RUN_EOF\n"
        "chmod +x {}/tccp_run.sh'",
        ssh_opts, compute_node, scratch, run_script, scratch);

    // Simpler approach: write to DTN first, then scp
    std::string dtn_script = fmt::format("/tmp/tccp_run_{}.sh", job_id);
    std::string heredoc_cmd = "cat > " + dtn_script + " << 'TCCP_RUN_EOF'\n"
                              + run_script + "\nTCCP_RUN_EOF";
    auto r = dtn_.run(heredoc_cmd);
    tccp_log_ssh("launch:write-script", "cat > " + dtn_script, r);

    dtn_.run("chmod +x " + dtn_script);

    // Copy script to compute node
    r = dtn_.run(fmt::format("scp {} {} {}:{}/tccp_run.sh",
                             ssh_opts, dtn_script, compute_node, scratch));
    tccp_log_ssh("launch:scp-script", "scp script to node", r);
    dtn_.run("rm -f " + dtn_script);

    // Create output symlink
    r = dtn_.run(fmt::format("ssh {} {} 'ln -sfn {} {}/output'",
                             ssh_opts, compute_node, out_dir, scratch));
    tccp_log_ssh("launch:output-symlink", "ln -sfn", r);

    // Launch dtach
    std::string launch_cmd = fmt::format(
        "ssh {} {} '$HOME/tccp/bin/dtach -n {} {}/tccp_run.sh'",
        ssh_opts, compute_node, sock, scratch);
    r = dtn_.run(launch_cmd);
    tccp_log_ssh("launch:dtach", launch_cmd, r);

    if (r.exit_code != 0) {
        return Result<void>::Err(fmt::format("Failed to launch dtach: {}", r.get_output()));
    }

    if (cb) cb(fmt::format("Job launched on {}", compute_node));
    return Result<void>::Ok();
}

// ── run ────────────────────────────────────────────────────

Result<TrackedJob> JobManager::run(const std::string& job_name, StatusCallback cb) {
    tccp_log("========================================");
    tccp_log(fmt::format("=== run job_name={} ===", job_name));

    // Validate job exists in config
    const auto& proj = config_.project();
    if (proj.jobs.find(job_name) == proj.jobs.end()) {
        return Result<TrackedJob>::Err("No job named '" + job_name + "' in tccp.yaml");
    }

    // Generate job ID and create tracked job immediately
    std::string job_id = generate_job_id(job_name);
    tccp_log(fmt::format("job_id={}", job_id));

    TrackedJob tj;
    tj.job_id = job_id;
    tj.job_name = job_name;
    tj.submit_time = now_iso();  // Record submission time
    tj.init_complete = false;  // Init will run in background

    // Add to tracked list and persist immediately
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        tracked_.push_back(tj);
    }

    JobState js;
    js.job_id = job_id;
    js.job_name = job_name;
    js.submit_time = tj.submit_time;
    js.init_complete = false;
    allocs_.state().jobs.push_back(js);
    allocs_.persist();

    if (cb) cb("Starting initialization in background...");

    // Spawn background thread to do all init work
    init_threads_.emplace_back(&JobManager::background_init_thread, this, job_id, job_name);
    init_threads_.back().detach();  // Let it run independently

    return Result<TrackedJob>::Ok(tj);
}

// ── Background init thread ─────────────────────────────────

void JobManager::background_init_thread(std::string job_id, std::string job_name) {
    tccp_log(fmt::format("INIT THREAD START: job_id={}", job_id));

    auto log_to_file = [&](const std::string& msg) {
        // Log init messages to a file that UI can tail
        std::string init_log = "/tmp/tccp_init_" + job_id + ".log";
        std::ofstream f(init_log, std::ios::app);
        if (f) {
            f << msg << "\n";
        }
        tccp_log(fmt::format("INIT: {}", msg));
    };

    auto check_canceled = [&]() -> bool {
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        return cancel_requested_.count(job_id) > 0;
    };

    try {
        // Check if canceled before starting
        if (check_canceled()) {
            log_to_file("Canceled before initialization started");
            throw std::runtime_error("Canceled during initialization");
        }

        // Resolve SLURM profile and job time
        auto profile = allocs_.resolve_profile(job_name);
        const auto& proj = config_.project();
        auto it = proj.jobs.find(job_name);
        std::string job_time = (it != proj.jobs.end() && !it->second.time.empty())
                               ? it->second.time : "1:00:00";
        int job_minutes = AllocationManager::parse_time_minutes(job_time);

        // Find or create allocation
        AllocationState* alloc = allocs_.find_free(job_minutes);
        if (alloc) {
            log_to_file(fmt::format("Reusing allocation {} on {}", alloc->slurm_id, alloc->node));
        } else {
            // Check for pending allocation
            AllocationState* pending = allocs_.find_pending();
            if (pending) {
                log_to_file(fmt::format("Found pending allocation {}, waiting for node...", pending->slurm_id));
                auto wait_result = allocs_.wait_for_allocation(pending->slurm_id, log_to_file);
                if (wait_result.is_ok()) {
                    alloc = allocs_.find_free(job_minutes);
                }
            }

            if (!alloc) {
                log_to_file("No free allocation, requesting new one...");
                auto alloc_result = allocs_.allocate(profile, log_to_file);
                if (alloc_result.is_err()) {
                    throw std::runtime_error(alloc_result.error);
                }
                alloc = allocs_.find_free(job_minutes);
                if (!alloc) {
                    throw std::runtime_error("Allocation succeeded but could not find it");
                }
            }
        }

        log_to_file(fmt::format("Using allocation {} on node {}", alloc->slurm_id, alloc->node));

        // Check cancellation after allocation acquired
        if (check_canceled()) {
            log_to_file("Canceled after allocation acquired (keeping allocation)");
            throw std::runtime_error("Canceled during initialization");
        }

        // Ensure directories and environment
        log_to_file("Setting up directories...");
        ensure_dirs(job_id, nullptr);

        if (check_canceled()) {
            log_to_file("Canceled during setup");
            throw std::runtime_error("Canceled during initialization");
        }

        log_to_file("Checking environment...");
        ensure_environment(nullptr);

        if (check_canceled()) {
            log_to_file("Canceled during environment setup");
            throw std::runtime_error("Canceled during initialization");
        }

        // Sync code to compute node
        std::string scratch = scratch_dir(job_id);
        log_to_file(fmt::format("Syncing code to {}...", alloc->node));
        sync_.sync_to_scratch(alloc->node, scratch, allocs_.state(), nullptr);

        if (check_canceled()) {
            log_to_file("Canceled after sync");
            throw std::runtime_error("Canceled during initialization");
        }

        // Launch job on compute node
        log_to_file("Launching job on compute node...");
        auto launch_result = launch_on_node(job_id, job_name, alloc->node, scratch, nullptr);
        if (launch_result.is_err()) {
            throw std::runtime_error(launch_result.error);
        }

        log_to_file(fmt::format("Job launched successfully on {}", alloc->node));

        // Update tracked job with final info
        std::string start = now_iso();
        {
            std::lock_guard<std::mutex> lock(tracked_mutex_);
            for (auto& tj : tracked_) {
                if (tj.job_id == job_id) {
                    tj.slurm_id = alloc->slurm_id;
                    tj.compute_node = alloc->node;
                    tj.scratch_path = scratch;
                    tj.init_complete = true;
                    tj.start_time = start;
                    break;
                }
            }
        }

        // Update persisted state
        for (auto& js : allocs_.state().jobs) {
            if (js.job_id == job_id) {
                js.alloc_slurm_id = alloc->slurm_id;
                js.compute_node = alloc->node;
                js.scratch_path = scratch;
                js.init_complete = true;
                js.start_time = start;
                break;
            }
        }
        allocs_.assign_job(alloc->slurm_id, job_id);  // This persists

        tccp_log(fmt::format("INIT THREAD COMPLETE: job_id={}", job_id));

        // Clean up cancel request flag
        {
            std::lock_guard<std::mutex> lock(cancel_mutex_);
            cancel_requested_.erase(job_id);
        }

    } catch (const std::exception& e) {
        std::string error = fmt::format("Init failed: {}", e.what());
        bool was_canceled = check_canceled();

        log_to_file(error);
        tccp_log(fmt::format("INIT THREAD ERROR: {}", error));

        // Mark job as failed/canceled
        {
            std::lock_guard<std::mutex> lock(tracked_mutex_);
            for (auto& tj : tracked_) {
                if (tj.job_id == job_id) {
                    if (!was_canceled) {
                        tj.init_error = error;
                    }
                    tj.init_complete = true;  // Done trying
                    break;
                }
            }

            // Clean up cancel request flag
            cancel_mutex_.lock();
            cancel_requested_.erase(job_id);
            cancel_mutex_.unlock();
        }

        for (auto& js : allocs_.state().jobs) {
            if (js.job_id == job_id) {
                if (!was_canceled) {
                    js.init_error = error;
                }
                js.init_complete = true;
                break;
            }
        }
        allocs_.persist();
    }
}

// ── list / cancel / return_output ──────────────────────────

SSHResult JobManager::list(StatusCallback cb) {
    if (cb) cb("Querying job status...");
    return login_.run("squeue -u " + username_ +
                      " -o \"%.8i %.20j %.10T %.6M %.4D %R\" -h");
}

// Shared cancel logic
Result<void> JobManager::do_cancel(TrackedJob* tj, const std::string& job_name) {
    // If job is still initializing, request cancellation
    if (!tj->init_complete) {
        // Mark for cancellation (init thread will check this)
        {
            std::lock_guard<std::mutex> lock(cancel_mutex_);
            cancel_requested_.insert(tj->job_id);
        }

        // Mark as canceled immediately
        tj->canceled = true;
        tj->completed = true;
        tj->exit_code = 130;  // 128 + SIGINT

        // Update persistent state
        auto& jobs = allocs_.state().jobs;
        for (auto& js : jobs) {
            if (js.job_id == tj->job_id) {
                js.canceled = true;
                js.completed = true;
                js.exit_code = 130;
                break;
            }
        }
        allocs_.persist();

        return Result<void>::Ok();
    }

    // Check REMOTE state (don't trust local completed flag)
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=3";
    std::string dtach_socket = fmt::format("/tmp/tccp_{}.sock", tj->job_id);

    // Use test -e on the socket file (same mechanism as view/attach)
    std::string check_cmd = fmt::format(
        "ssh {} {} 'test -e {} && echo RUNNING || echo DONE'",
        ssh_opts, tj->compute_node, dtach_socket);

    auto check_result = dtn_.run(check_cmd);
    tccp_log_ssh("cancel:check-remote", check_cmd, check_result);

    // If remote shows job is done, update local state and return error
    if (check_result.stdout_data.find("DONE") != std::string::npos) {
        tj->completed = true;

        // Persist the updated state
        auto& jobs = allocs_.state().jobs;
        for (auto& js : jobs) {
            if (js.job_id == tj->job_id) {
                js.completed = true;
                break;
            }
        }
        allocs_.persist();

        if (tj->canceled) {
            return Result<void>::Err("Job '" + job_name + "' already canceled");
        } else {
            return Result<void>::Err(fmt::format("Job '{}' already completed (exit {})", job_name, tj->exit_code));
        }
    }

    // Kill the dtach process using fuser (finds process owning the socket)
    // Then remove the socket file to clean up
    std::string kill_cmd = fmt::format(
        "ssh {} {} 'fuser -k -9 {} 2>/dev/null; rm -f {}'",
        ssh_opts, tj->compute_node, dtach_socket, dtach_socket);

    auto kill_result = dtn_.run(kill_cmd);
    tccp_log_ssh("cancel:kill", kill_cmd, kill_result);

    // Mark as canceled
    tj->canceled = true;
    tj->completed = true;
    tj->exit_code = 130;  // 128 + SIGINT

    // Update persistent state
    auto& jobs = allocs_.state().jobs;
    for (auto& js : jobs) {
        if (js.job_id == tj->job_id) {
            js.canceled = true;
            js.completed = true;
            js.exit_code = 130;
            break;
        }
    }

    // Free the allocation's active job slot (but keep allocation running)
    // Only if an allocation was actually assigned
    if (!tj->slurm_id.empty()) {
        allocs_.release_job(tj->slurm_id);  // This calls persist()
    } else {
        allocs_.persist();  // Just persist the canceled state
    }

    return Result<void>::Ok();
}

Result<void> JobManager::cancel_job(const std::string& job_name, StatusCallback cb) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);

    // Find the most recent job by name (last match = newest)
    TrackedJob* tj = nullptr;
    for (auto& job : tracked_) {
        if (job.job_name == job_name) {
            tj = &job;
        }
    }

    if (!tj) {
        return Result<void>::Err("Job '" + job_name + "' not found");
    }

    if (cb) cb(fmt::format("Canceling job '{}'...", job_name));

    auto result = do_cancel(tj, job_name);

    if (result.is_ok() && cb) {
        if (tj->slurm_id.empty()) {
            cb(fmt::format("Job '{}' canceled during initialization", job_name));
        } else {
            cb(fmt::format("Job '{}' canceled (allocation {} kept alive)", job_name, tj->slurm_id));
        }
    }

    return result;
}

Result<void> JobManager::cancel_job_by_id(const std::string& job_id, StatusCallback cb) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);

    // Find job by ID
    TrackedJob* tj = nullptr;
    for (auto& job : tracked_) {
        if (job.job_id == job_id) {
            tj = &job;
            break;
        }
    }

    if (!tj) {
        return Result<void>::Err("Job ID '" + job_id + "' not found");
    }

    if (cb) cb(fmt::format("Canceling job '{}'...", tj->job_name));

    auto result = do_cancel(tj, tj->job_name);

    if (result.is_ok() && cb) {
        if (tj->slurm_id.empty()) {
            cb(fmt::format("Job '{}' canceled during initialization", tj->job_name));
        } else {
            cb(fmt::format("Job '{}' canceled", tj->job_name));
        }
    }

    return result;
}

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
    return Result<void>::Ok();
}

// ── Poll ───────────────────────────────────────────────────

JobManager::SlurmJobState JobManager::query_alloc_state(const std::string& slurm_id) {
    std::string cmd = fmt::format("squeue -j {} -o \"%T %N\" -h", slurm_id);
    auto result = login_.run(cmd);

    SlurmJobState state;
    if (result.success()) {
        std::istringstream iss(result.stdout_data);
        iss >> state.state >> state.node;
    }
    return state;
}

void JobManager::poll(std::function<void(const TrackedJob&)> on_complete) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
    for (auto& tj : tracked_) {
        if (tj.completed) continue;
        if (!tj.init_complete) continue;  // Still initializing, skip

        // Check if the dtach socket still exists (job still running)
        const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=3";
        std::string sock = "/tmp/tccp_" + tj.job_id + ".sock";
        auto check = dtn_.run(fmt::format("ssh {} {} 'test -e {} && echo RUNNING || echo DONE'",
                                          ssh_opts, tj.compute_node, sock));

        if (check.stdout_data.find("DONE") != std::string::npos) {
            tj.completed = true;
            tj.exit_code = 0; // will be refined by the attach loop
            on_job_complete(tj);
            if (on_complete) on_complete(tj);
        }

        // Also check allocation is still alive
        if (tj.compute_node.empty()) {
            auto state = query_alloc_state(tj.slurm_id);
            if (state.state == "RUNNING" && !state.node.empty()) {
                tj.compute_node = state.node;
            } else if (state.state.empty() || state.state == "COMPLETED" ||
                       state.state == "FAILED" || state.state == "CANCELLED") {
                tj.completed = true;
                if (on_complete) on_complete(tj);
            }
        }
    }
}

void JobManager::on_job_complete(TrackedJob& tj) {
    // Release the allocation so it can be reused
    allocs_.release_job(tj.slurm_id);

    // Set end time
    tj.end_time = now_iso();

    // Update persistent state
    auto& jobs = allocs_.state().jobs;
    for (auto& js : jobs) {
        if (js.job_id == tj.job_id) {
            js.completed = tj.completed;
            js.exit_code = tj.exit_code;
            js.output_file = tj.output_file;
            js.end_time = tj.end_time;
            break;
        }
    }
}

const std::vector<TrackedJob>& JobManager::tracked_jobs() const {
    return tracked_;
}

TrackedJob* JobManager::find_by_name(const std::string& job_name) {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
    TrackedJob* best = nullptr;
    for (auto& tj : tracked_) {
        if (tj.job_name == job_name) {
            best = &tj;
        }
    }
    return best;
}
