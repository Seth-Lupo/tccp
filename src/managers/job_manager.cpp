#include "job_manager.hpp"
#include "gitignore.hpp"
#include "gpu_discovery.hpp"
#include <core/credentials.hpp>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <set>
#include <map>
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
        tj.output_returned = js.output_returned;
        tj.submit_time = js.submit_time;
        tj.start_time = js.start_time;
        tj.end_time = js.end_time;
        tracked_.push_back(tj);
    }
}

JobManager::~JobManager() {
    // Signal all background init threads to stop
    shutdown_.store(true);

    // Join all joinable threads (with a short timeout via detach fallback)
    for (auto& t : init_threads_) {
        if (t.joinable()) {
            t.detach();  // Can't block indefinitely; threads check shutdown_ flag
        }
    }
    init_threads_.clear();
}

void JobManager::clear_tracked() {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
    tracked_.clear();
    environment_checked_ = false;
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

// ── Container/environment helpers (type-aware) ──────────────

std::string JobManager::container_docker_uri() const {
    const auto& type = config_.project().type;
    if (type == "python-pytorch") {
        return "docker://pytorch/pytorch:2.6.0-cuda12.4-cudnn9-runtime";
    }
    return "docker://python:3.11-slim";
}

std::string JobManager::container_image_path() const {
    std::string cc = container_cache();
    const auto& type = config_.project().type;
    if (type == "python-pytorch") {
        return cc + "/images/pytorch_2.6.0-cuda12.4-runtime.sif";
    }
    return cc + "/images/python_3.11-slim.sif";
}

bool JobManager::is_gpu_type() const {
    const auto& type = config_.project().type;
    return type == "python-pytorch";
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
    // Skip check if already validated this session
    if (environment_checked_) {
        return;
    }

    if (cb) cb("Checking environment...");

    std::string cc = container_cache();
    std::string image = container_image_path();
    std::string docker_uri = container_docker_uri();
    std::string venv = env_dir() + "/default/venv";
    std::string tccp_home = fmt::format("/cluster/home/{}/tccp", username_);
    std::string dtach_bin = tccp_home + "/bin/dtach";
    bool gpu = is_gpu_type();

    // Single check for all three components
    std::string check_all = fmt::format(
        "test -f {} && echo IMAGE_OK || echo IMAGE_MISSING; "
        "test -f {}/bin/python && echo VENV_OK || echo VENV_MISSING; "
        "test -x {} && echo DTACH_OK || echo DTACH_MISSING",
        image, venv, dtach_bin);

    auto result = dtn_.run(check_all);
    bool need_image = result.stdout_data.find("IMAGE_MISSING") != std::string::npos;
    bool need_venv = result.stdout_data.find("VENV_MISSING") != std::string::npos;
    bool need_dtach = result.stdout_data.find("DTACH_MISSING") != std::string::npos;

    // Show what we found
    if (!need_image && cb) cb("Container image: OK");
    if (!need_venv && cb) cb("Python venv: OK");
    if (!need_dtach && cb) cb("dtach: OK");

    // Only load modules if we need to create something
    if (need_image || need_venv) {
        if (cb) cb("Loading Singularity module...");
        dtn_.run("module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true");
    }

    // Pull container image if missing
    if (need_image) {
        if (cb) cb(fmt::format("Pulling container image {} (first time only)...", docker_uri));
        dtn_.run(fmt::format("mkdir -p {}/images {}/cache {}/tmp", cc, cc, cc));
        std::string pull_cmd = fmt::format(
            "SINGULARITY_CACHEDIR={0}/cache "
            "SINGULARITY_TMPDIR={0}/tmp "
            "singularity pull {1} {2}", cc, image, docker_uri);
        auto pull_result = dtn_.run(pull_cmd);
        // Verify the image was actually pulled
        auto img_check = dtn_.run("test -f " + image + " && echo IMG_OK || echo IMG_FAIL");
        if (img_check.stdout_data.find("IMG_FAIL") != std::string::npos) {
            throw std::runtime_error(fmt::format("Container pull failed: {}", pull_result.get_output()));
        }
        if (cb) cb("Container image: OK");
    }

    // Create venv if missing
    if (need_venv) {
        if (cb) cb("Creating Python virtual environment...");
        std::string base = persistent_base();
        // For GPU types (pytorch, etc.), use --system-site-packages so the
        // container's pre-installed packages (PyTorch, CUDA, etc.) are visible
        std::string venv_flags = gpu ? "--system-site-packages " : "";
        std::string nv_flag = gpu ? "--nv " : "";
        std::string venv_cmd = fmt::format(
            "singularity exec {}--bind {}:{} {} python -m venv {}{}",
            nv_flag, base, base, image, venv_flags, venv);
        dtn_.run(venv_cmd);
        if (cb) cb("Python venv: OK");
    }

    // Build dtach if missing
    if (need_dtach) {
        if (cb) cb("Building dtach...");
        ensure_dtach(cb);
        if (cb) cb("dtach: OK");
    }

    // Mark as checked for this session
    environment_checked_ = true;
    if (cb) cb("Environment OK");
}

void JobManager::ensure_dtach(StatusCallback cb) {
    std::string tccp_home = fmt::format("/cluster/home/{}/tccp", username_);
    std::string bin = tccp_home + "/bin/dtach";

    auto check = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_MISSING");
    if (check.stdout_data.find("DTACH_OK") != std::string::npos) {
        return;
    }

    // Check if dtach is already available system-wide
    auto sys_check = dtn_.run("which dtach 2>/dev/null");
    if (sys_check.exit_code == 0 && !sys_check.stdout_data.empty()) {
        std::string sys_dtach = sys_check.stdout_data;
        // trim newline
        while (!sys_dtach.empty() && (sys_dtach.back() == '\n' || sys_dtach.back() == '\r'))
            sys_dtach.pop_back();
        dtn_.run("mkdir -p " + tccp_home + "/bin");
        dtn_.run("cp " + sys_dtach + " " + bin + " && chmod +x " + bin);
        auto v = dtn_.run("test -x " + bin + " && echo OK");
        if (v.stdout_data.find("OK") != std::string::npos) {
            if (cb) cb("dtach: copied from system");
            return;
        }
    }

    if (cb) cb("Building dtach (first time only)...");

    std::string build_dir = tccp_home + "/dtach-build";
    dtn_.run("mkdir -p " + tccp_home + "/bin");
    dtn_.run("rm -rf " + build_dir);

    auto clone = dtn_.run("git clone https://github.com/crigler/dtach.git " + build_dir + " 2>&1");
    if (clone.exit_code != 0) {
        // git might not be available — try curl
        if (cb) cb("git not available, trying curl...");
        dtn_.run("mkdir -p " + build_dir);
        auto dl = dtn_.run("curl -sL https://github.com/crigler/dtach/archive/refs/heads/master.tar.gz "
                           "| tar xz -C " + build_dir + " --strip-components=1 2>&1");
        if (dl.exit_code != 0) {
            throw std::runtime_error(fmt::format(
                "Cannot download dtach source (no git or curl?): {}", dl.get_output()));
        }
    }

    auto build = dtn_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1");
    if (build.exit_code != 0) {
        if (cb) cb("Direct compile failed, trying configure/make...");
        auto build2 = dtn_.run("cd " + build_dir + " && ./configure && make 2>&1");
        if (build2.exit_code != 0) {
            throw std::runtime_error(fmt::format(
                "Failed to compile dtach: {}", build2.get_output()));
        }
    }

    dtn_.run("cp " + build_dir + "/dtach " + bin + " && chmod +x " + bin);
    dtn_.run("rm -rf " + build_dir);

    auto verify = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_FAIL");
    if (verify.stdout_data.find("DTACH_OK") != std::string::npos) {
        if (cb) cb("dtach built successfully");
    } else {
        throw std::runtime_error("Failed to install dtach binary");
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
    std::string image = container_image_path();
    std::string venv = env_dir() + "/default/venv";
    std::string out_dir = job_output_dir(job_id);
    std::string sock = "/tmp/tccp_" + job_id + ".sock";
    std::string log = "/tmp/tccp_" + job_id + ".log";
    bool gpu = is_gpu_type();
    std::string nv_flag = gpu ? "--nv " : "";

    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";

    if (cb) cb("Launching job on compute node...");

    // Build bind mount flags (--nv for GPU types enables NVIDIA driver access)
    std::string binds = fmt::format("{}--bind {}:{} --bind {}:{}",
                                    nv_flag, scratch, scratch, venv, venv);
    if (!proj.cache.empty()) {
        std::string shared_cache = fmt::format("/tmp/{}/{}/.tccp-cache",
                                               username_, proj.name);
        binds += fmt::format(" --bind {}:{}/cache", shared_cache, scratch);
    }

    // Create the run script on the compute node
    // For GPU types (python-pytorch), --nv is in binds and the venv was created
    // with --system-site-packages so {venv}/bin/python sees container packages
    std::string run_script = fmt::format(
        "#!/bin/bash\n"
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true\n"
        "export SINGULARITY_CACHEDIR={}/cache\n"
        "export SINGULARITY_TMPDIR={}/tmp\n"
        "cd {}\n"
        "if [ -f requirements.txt ]; then\n"
        "    REQ_HASH=$(md5sum requirements.txt 2>/dev/null | cut -d' ' -f1)\n"
        "    STAMP={}/env/.req_installed\n"
        "    if [ ! -f \"$STAMP\" ] || [ \"$(cat $STAMP 2>/dev/null)\" != \"$REQ_HASH\" ]; then\n"
        "        singularity exec {} {} {}/bin/pip install -q -r requirements.txt\n"
        "        echo \"$REQ_HASH\" > \"$STAMP\"\n"
        "    fi\n"
        "fi\n"
        "printf '\\n__TCCP_JOB_START__\\n'\n"
        "CMD=\"singularity exec {} {} {}/bin/python {} {}\"\n"
        "script -fq -c \"$CMD\" {}\n"
        "EC=$?\n"
        "printf '\\033[J'\n"
        "exit $EC\n",
        cc, cc,
        scratch,
        scratch, binds, image, venv,
        binds, image, venv, script, args,
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

    // Create shared cache directory (persists across jobs on same allocation, bind-mounted into container)
    if (!proj.cache.empty()) {
        std::string shared_cache = fmt::format("/tmp/{}/{}/.tccp-cache",
                                               username_, proj.name);
        r = dtn_.run(fmt::format("ssh {} {} 'mkdir -p {}'",
                                 ssh_opts, compute_node, shared_cache));
        tccp_log_ssh("launch:cache-dir", "mkdir cache", r);
    }

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
        if (shutdown_.load()) return true;
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

        // Resolve GPU partition early so find_free() and allocate() both
        // use the same partition (not the default "batch")
        if (profile.gpu_count > 0 || !profile.gpu_type.empty()) {
            log_to_file("Resolving GPU partition...");
            auto match = resolve_gpu_partition(login_, username_,
                                               profile.partition,
                                               profile.gpu_type,
                                               profile.gpu_count);
            if (match.found) {
                profile.partition = match.partition;
                if (profile.gpu_type.empty()) {
                    profile.gpu_type = match.gpu_type;
                }
                log_to_file(fmt::format("GPU partition: {} ({})", match.partition, match.gpu_type));
            }
        }

        // Poll for recently completed jobs so their allocations become free
        poll(nullptr);

        // Find or create allocation
        AllocationState* alloc = allocs_.find_free(job_minutes, profile, log_to_file);
        if (alloc) {
            log_to_file(fmt::format("Reusing allocation {} on {}", alloc->slurm_id, alloc->node));
        } else {
            // Check for pending allocation
            AllocationState* pending = allocs_.find_pending();
            if (pending) {
                log_to_file(fmt::format("Found pending allocation {}, waiting for node...", pending->slurm_id));
                auto wait_result = allocs_.wait_for_allocation(pending->slurm_id, log_to_file);
                if (wait_result.is_ok()) {
                    alloc = allocs_.find_by_id(wait_result.value.slurm_id);
                }
            }

            if (!alloc) {
                log_to_file("No free allocation, requesting new one...");
                auto alloc_result = allocs_.allocate(profile, log_to_file);
                if (alloc_result.is_err()) {
                    throw std::runtime_error(alloc_result.error);
                }
                alloc = allocs_.find_by_id(alloc_result.value.slurm_id);
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

        ensure_environment(log_to_file);

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
        tj->exit_code = 130;
        tj->end_time = now_iso();
        persist_job_state(*tj);
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
        persist_job_state(*tj);
        allocs_.persist();

        if (tj->canceled) {
            return Result<void>::Err("Job '" + job_name + "' already canceled");
        } else {
            return Result<void>::Err(fmt::format("Job '{}' already completed (exit {})", job_name, tj->exit_code));
        }
    }

    // Kill the dtach process
    std::string kill_cmd = fmt::format(
        "ssh {} {} 'fuser -k -9 {} 2>/dev/null'",
        ssh_opts, tj->compute_node, dtach_socket);
    auto kill_result = dtn_.run(kill_cmd);
    tccp_log_ssh("cancel:kill", kill_cmd, kill_result);

    // Mark as canceled and finalize
    tj->canceled = true;
    tj->completed = true;
    tj->exit_code = 130;  // 128 + SIGINT
    tj->end_time = now_iso();

    cleanup_compute_node(*tj);
    persist_job_state(*tj);

    if (!tj->slurm_id.empty()) {
        allocs_.release_job(tj->slurm_id);
    } else {
        allocs_.persist();
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
    // Phase 1: Detect completed jobs while holding the lock.
    // Only update fields on tracked_ entries; do NOT call on_job_complete here
    // (it calls prune_completed_jobs which modifies tracked_, causing iterator invalidation).
    std::vector<TrackedJob> newly_completed;

    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& tj : tracked_) {
            if (tj.completed) continue;
            if (!tj.init_complete) continue;  // Still initializing, skip

            if (!tj.compute_node.empty()) {
                // Job has a compute node — check if dtach socket still exists
                const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=3";
                std::string sock = "/tmp/tccp_" + tj.job_id + ".sock";
                auto check = dtn_.run(fmt::format("ssh {} {} 'test -e {} && echo RUNNING || echo DONE'",
                                                  ssh_opts, tj.compute_node, sock));

                if (check.stdout_data.find("DONE") != std::string::npos) {
                    tj.completed = true;
                    tj.exit_code = 0;
                    tj.end_time = now_iso();
                    newly_completed.push_back(tj);
                } else if (check.stdout_data.find("RUNNING") == std::string::npos) {
                    // SSH failed entirely — check if allocation was killed
                    auto alloc_state = query_alloc_state(tj.slurm_id);
                    if (alloc_state.state.empty() || alloc_state.state == "COMPLETED" ||
                        alloc_state.state == "FAILED" || alloc_state.state == "CANCELLED") {
                        tj.completed = true;
                        tj.exit_code = -1;
                        tj.end_time = now_iso();
                        newly_completed.push_back(tj);
                        tccp_log(fmt::format("poll: allocation {} gone ({}), marking job {} complete",
                                             tj.slurm_id, alloc_state.state, tj.job_id));
                    }
                }
            } else {
                // No compute node yet — check if allocation is still alive
                auto alloc_state = query_alloc_state(tj.slurm_id);
                if (alloc_state.state == "RUNNING" && !alloc_state.node.empty()) {
                    tj.compute_node = alloc_state.node;
                } else if (alloc_state.state.empty() || alloc_state.state == "COMPLETED" ||
                           alloc_state.state == "FAILED" || alloc_state.state == "CANCELLED") {
                    tj.completed = true;
                    tj.exit_code = -1;
                    tj.end_time = now_iso();
                    newly_completed.push_back(tj);
                }
            }
        }
    }

    // Phase 2: Process completions without the lock held.
    // release_job, cleanup, persist, and try_return all work on allocs state
    // (try_return_output self-locks tracked_mutex_ for its own updates)
    for (auto& tj : newly_completed) {
        allocs_.release_job(tj.slurm_id);
        cleanup_compute_node(tj);
        persist_job_state(tj);
        try_return_output(tj);
    }

    // Prune once after all completions (self-locks tracked_mutex_)
    if (!newly_completed.empty()) {
        prune_completed_jobs();
    }

    // Phase 3: User callbacks
    for (const auto& tj : newly_completed) {
        if (on_complete) on_complete(tj);
    }
}

void JobManager::cleanup_compute_node(const TrackedJob& tj) {
    if (tj.compute_node.empty()) return;
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";
    // Remove scratch dir and dtach socket; preserve .log for viewing
    std::string cmd = fmt::format("ssh {} {} 'rm -rf {} /tmp/tccp_{}.sock'",
                                  ssh_opts, tj.compute_node, tj.scratch_path, tj.job_id);
    dtn_.run(cmd);
}

void JobManager::persist_job_state(const TrackedJob& tj) {
    auto& jobs = allocs_.state().jobs;
    for (auto& js : jobs) {
        if (js.job_id == tj.job_id) {
            js.completed = tj.completed;
            js.canceled = tj.canceled;
            js.exit_code = tj.exit_code;
            js.output_file = tj.output_file;
            js.end_time = tj.end_time;
            break;
        }
    }
}

void JobManager::on_job_complete(TrackedJob& tj) {
    // NOTE: This must be called WITHOUT tracked_mutex_ held.
    // It is safe to call from outside poll() (e.g. view/attach session end).
    allocs_.release_job(tj.slurm_id);
    if (tj.end_time.empty()) tj.end_time = now_iso();
    cleanup_compute_node(tj);
    persist_job_state(tj);
    try_return_output(tj);
    prune_completed_jobs();
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

void JobManager::prune_completed_jobs() {
    auto& state_jobs = allocs_.state().jobs;

    // Group completed/canceled/failed jobs by name, keep only latest
    std::map<std::string, size_t> latest_by_name;
    std::vector<bool> keep(state_jobs.size(), false);

    for (size_t i = 0; i < state_jobs.size(); i++) {
        auto& js = state_jobs[i];
        bool is_terminal = js.completed || js.canceled || !js.init_error.empty();
        if (!is_terminal) {
            keep[i] = true;  // Always keep active jobs
            continue;
        }
        auto it = latest_by_name.find(js.job_name);
        if (it == latest_by_name.end()) {
            latest_by_name[js.job_name] = i;
        } else {
            if (js.submit_time > state_jobs[it->second].submit_time) {
                latest_by_name[js.job_name] = i;
            }
        }
    }
    for (auto& [name, idx] : latest_by_name) keep[idx] = true;

    // Compact state jobs
    std::vector<JobState> kept_jobs;
    std::set<std::string> kept_ids;
    for (size_t i = 0; i < state_jobs.size(); i++) {
        if (keep[i]) {
            kept_jobs.push_back(state_jobs[i]);
            kept_ids.insert(state_jobs[i].job_id);
        }
    }

    if (kept_jobs.size() < state_jobs.size()) {
        state_jobs = kept_jobs;

        // Prune tracked_ to match (lock since other threads read tracked_)
        {
            std::lock_guard<std::mutex> lock(tracked_mutex_);
            std::vector<TrackedJob> kept_tracked;
            for (auto& tj : tracked_) {
                if (kept_ids.count(tj.job_id)) {
                    kept_tracked.push_back(tj);
                }
            }
            tracked_ = kept_tracked;
        }

        allocs_.persist();
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
