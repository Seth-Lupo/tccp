#include "job_manager.hpp"
#include "job_log.hpp"
#include "gpu_discovery.hpp"
#include <environments/environment.hpp>
#include <core/constants.hpp>
#include <core/utils.hpp>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <set>
#include <filesystem>

namespace fs = std::filesystem;

// ── Constructor ────────────────────────────────────────────

JobManager::JobManager(const Config& config, ConnectionFactory& factory,
                       AllocationManager& allocs, SyncManager& sync, CacheManager& cache)
    : config_(config), factory_(factory),
      dtn_(factory.dtn()), login_(factory.login()),
      allocs_(allocs), sync_(sync), cache_(cache),
      username_(get_cluster_username()),
      port_fwd_(factory) {

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
        tj.forwarded_ports = js.forwarded_ports;
        tj.submit_time = js.submit_time;
        tj.start_time = js.start_time;
        tj.end_time = js.end_time;
        tracked_.push_back(tj);
    }

    // Re-establish port forwarding tunnels for running jobs
    for (auto& tj : tracked_) {
        if (!tj.completed && tj.init_complete && !tj.compute_node.empty()
            && !tj.forwarded_ports.empty()) {
            tj.tunnel_pids = port_fwd_.start(tj.compute_node, tj.forwarded_ports);
        }
    }

}

JobManager::~JobManager() {
    // Signal all background init threads to stop
    shutdown_.store(true);

    // Stop all tunnel processes
    for (auto& tj : tracked_) {
        if (!tj.tunnel_pids.empty()) {
            PortForwarder::stop(tj.tunnel_pids);
            tj.tunnel_pids.clear();
        }
    }

    // Join all init threads — they check shutdown_ flag and will exit promptly.
    // Must complete before ConnectionFactory is destroyed, or they'll segfault
    // on dangling dtn_/login_ references.
    for (auto& t : init_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    init_threads_.clear();
}

void JobManager::clear_tracked() {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
    tracked_.clear();
    environment_checked_ = false;
}

// ── Path helpers ───────────────────────────────────────────

std::string JobManager::persistent_base() const {
    return fmt::format(REMOTE_PROJECT_BASE, username_, config_.project().name);
}

std::string JobManager::job_output_dir(const std::string& job_id) const {
    return persistent_base() + "/output/" + job_id;
}

std::string JobManager::env_dir() const {
    return persistent_base() + "/env";
}

std::string JobManager::container_cache() const {
    return fmt::format(REMOTE_CONTAINER_CACHE, username_);
}

std::string JobManager::scratch_dir(const std::string& job_id) const {
    return fmt::format(REMOTE_SCRATCH_DIR, username_, config_.project().name, job_id);
}

std::string JobManager::ssh_to_node(const std::string& node, const std::string& cmd) {
    return fmt::format("ssh {} {} '{}'", SSH_OPTS, node, cmd);
}

bool JobManager::is_implicit_job(const std::string& job_name) {
    // Add new implicit job types here (e.g. "jupyter", "tensorboard")
    return job_name == "bash";
}

JobManager::JobEnvPaths JobManager::resolve_env_paths(const std::string& job_id,
                                                       const std::string& scratch) const {
    const auto& env = get_environment(config_.project().type);
    std::string cc = container_cache();
    std::string nv_flag = env.gpu ? "--nv " : "";
    std::string venv = env_dir() + "/default/venv";

    JobEnvPaths p;
    p.image = cc + "/images/" + env.sif_filename;
    p.venv = venv;
    p.log = scratch + "/tccp.log";
    p.has_python = (env.type_name.find("python") != std::string::npos);

    // Bind mounts
    p.binds = fmt::format("{}--bind {}:{} --bind {}:{}",
                           nv_flag, scratch, scratch, venv, venv);
    if (!config_.project().cache.empty()) {
        std::string shared_cache = fmt::format("/tmp/{}/{}/.tccp-cache",
                                               username_, config_.project().name);
        p.binds += fmt::format(" --bind {}:{}/cache", shared_cache, scratch);
    }

    // Runtime cache
    if (!config_.project().cache.empty()) {
        p.runtime_cache = fmt::format("/tmp/{}/{}/.tccp-cache", username_, config_.project().name);
    } else {
        p.runtime_cache = scratch + "/.tccp-tmp";
    }

    return p;
}

std::string JobManager::build_run_preamble(const JobEnvPaths& paths,
                                            const std::string& scratch) const {
    std::string preamble = fmt::format(
        "#!/bin/bash\n"
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true\n"
        "export SINGULARITY_CACHEDIR={rc}/cache\n"
        "export SINGULARITY_TMPDIR={rc}/tmp\n"
        "cd {scratch}\n"
        "mkdir -p {rc}\n"
        "export HOME={rc}\n"
        "export XDG_CACHE_HOME={rc}/xdg-cache\n"
        "export TMPDIR={rc}/tmp\n"
        "export PIP_CACHE_DIR={rc}/pip-cache\n"
        "export HF_HOME={rc}/huggingface\n"
        "export TORCH_HOME={rc}/torch\n"
        "mkdir -p $XDG_CACHE_HOME $TMPDIR $PIP_CACHE_DIR $HF_HOME $TORCH_HOME\n",
        fmt::arg("rc", paths.runtime_cache),
        fmt::arg("scratch", scratch));

    // Custom environment variables from tccp.yaml
    for (const auto& [key, val] : config_.project().environment) {
        preamble += fmt::format("export {}='{}'\n", key, val);
    }

    preamble += "printf '\\n__TCCP_JOB_START__\\n'\n";

    return preamble;
}

std::string JobManager::build_job_payload(const std::string& job_name,
                                           const JobEnvPaths& paths,
                                           const std::string& extra_args) const {
    // ── Implicit job types ──────────────────────────────────
    // Add new cases here for future implicit jobs (e.g. "jupyter", "tensorboard")
    if (job_name == "bash") {
        return fmt::format(
            "echo 'Interactive session ready. Use ssh bash to connect.'\n"
            "singularity exec {} {} sleep infinity\n",
            paths.binds, paths.image);
    }

    // ── Normal script/package jobs ──────────────────────────
    const auto& proj = config_.project();
    auto it = proj.jobs.find(job_name);
    std::string args = (it != proj.jobs.end()) ? it->second.args : "";
    // Append runtime CLI args after config args
    if (!extra_args.empty()) {
        if (!args.empty()) args += " ";
        args += extra_args;
    }

    // python -m package  or  python script.py
    std::string python_cmd;
    if (it != proj.jobs.end() && !it->second.package.empty()) {
        python_cmd = fmt::format("{}/bin/python -m {} {}",
                                  paths.venv, it->second.package, args);
    } else {
        std::string script = (it != proj.jobs.end() && !it->second.script.empty())
                                 ? it->second.script : "main.py";
        python_cmd = fmt::format("{}/bin/python {} {}",
                                  paths.venv, script, args);
    }

    return fmt::format(
        "CMD=\"singularity exec {} {} {}\"\n"
        "script -fq -c \"$CMD\" {}\n"
        "EC=$?\n"
        "printf '\\033[J'\n"
        "cp {} output/tccp.log 2>/dev/null\n"
        "exit $EC\n",
        paths.binds, paths.image, python_cmd, paths.log,
        paths.log);
}

std::string JobManager::shell_command(const TrackedJob& tj, const std::string& inner_cmd) const {
    auto paths = resolve_env_paths(tj.job_id, tj.scratch_path);

    // All env vars go before singularity exec — they propagate into the
    // container, avoiding single-quote nesting issues across SSH hops.
    std::string env_setup = fmt::format(
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
        "cd {}; "
        "export HOME={}; "
        "export XDG_CACHE_HOME={}/xdg-cache; "
        "export TMPDIR={}/tmp; "
        "export PIP_CACHE_DIR={}/pip-cache; "
        "export HF_HOME={}/huggingface; "
        "export TORCH_HOME={}/torch; ",
        tj.scratch_path,
        paths.runtime_cache,
        paths.runtime_cache, paths.runtime_cache, paths.runtime_cache,
        paths.runtime_cache, paths.runtime_cache);

    // Custom environment variables from tccp.yaml
    for (const auto& [key, val] : config_.project().environment) {
        env_setup += fmt::format("export {}='{}'; ", key, val);
    }

    // Activate venv for python environments so bare `python` and `pip` work
    if (paths.has_python) {
        env_setup += fmt::format("export PATH={}/bin:$PATH; "
                                 "export VIRTUAL_ENV={}; ",
                                 paths.venv, paths.venv);
    }

    if (inner_cmd.empty()) {
        // Interactive shell: use SINGULARITYENV_PS1 so Singularity injects it
        // into the container (overriding its default "Singularity>" prompt).
        // bash --norc prevents .bashrc from overriding it.
        env_setup += fmt::format("export SINGULARITYENV_PS1=\"{}@{}> \"; ", tj.job_name, tj.compute_node);
        return env_setup + fmt::format("singularity exec {} {} bash --norc",
                                        paths.binds, paths.image);
    } else {
        // One-off command: run inside bash -c (single quotes around inner_cmd are safe
        // here since inner_cmd comes from user input, not our generated code)
        return env_setup + fmt::format("singularity exec {} {} bash -c '{}'",
                                        paths.binds, paths.image, inner_cmd);
    }
}

std::string JobManager::job_name_from_id(const std::string& job_id) {
    auto pos = job_id.find("__");
    if (pos != std::string::npos && pos + 2 < job_id.size()) {
        return job_id.substr(pos + 2);
    }
    return job_id;
}

std::string JobManager::timestamp_from_id(const std::string& job_id) {
    auto pos = job_id.find("__");
    if (pos != std::string::npos) {
        return job_id.substr(0, pos);
    }
    return job_id;
}

std::string JobManager::generate_job_id(const std::string& job_name) const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif

    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}-{:02d}-{:02d}-{:03d}__{}",
                       tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                       tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                       static_cast<int>(ms.count()), job_name);
}

// ── list ───────────────────────────────────────────────────

SSHResult JobManager::list(StatusCallback cb) {
    if (cb) cb("Querying job status...");
    return login_.run("squeue -u " + username_ +
                      " -o \"%.8i %.20j %.10T %.6M %.4D %R\" -h");
}

// ── run ────────────────────────────────────────────────────

Result<TrackedJob> JobManager::run(const std::string& job_name, const std::string& extra_args,
                                   StatusCallback cb) {
    tccp_log("========================================");
    tccp_log(fmt::format("=== run job_name={} extra_args='{}' ===", job_name, extra_args));

    // Validate job exists in config (implicit jobs like "bash" are always available)
    const auto& proj = config_.project();
    if (!is_implicit_job(job_name) && proj.jobs.find(job_name) == proj.jobs.end()) {
        return Result<TrackedJob>::Err("No job named '" + job_name + "' in tccp.yaml");
    }

    // Generate job ID and create tracked job immediately
    std::string job_id = generate_job_id(job_name);
    tccp_log(fmt::format("job_id={}", job_id));

    TrackedJob tj;
    tj.job_id = job_id;
    tj.job_name = job_name;
    tj.extra_args = extra_args;
    tj.submit_time = now_iso();
    tj.init_complete = false;

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
    init_threads_.emplace_back(&JobManager::background_init_thread, this, job_id, job_name, extra_args);

    return Result<TrackedJob>::Ok(tj);
}

// ── Background init thread ─────────────────────────────────

void JobManager::background_init_thread(std::string job_id, std::string job_name, std::string extra_args) {
    tccp_log(fmt::format("INIT THREAD START: job_id={}", job_id));

    // Ensure persistent log directory exists
    std::string log_path = job_log_path(job_id);
    fs::create_directories(fs::path(log_path).parent_path());

    auto t0 = std::chrono::steady_clock::now();
    auto log_to_file = [&](const std::string& msg) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::string ts = (ms < 1000) ? fmt::format("{}ms", ms) : fmt::format("{:.1f}s", ms / 1000.0);
        std::string stamped = fmt::format("[{}] {}", ts, msg);
        std::ofstream f(log_path, std::ios::app);
        if (f) {
            f << stamped << "\n";
        }
        tccp_log(fmt::format("INIT: {}", stamped));
    };

    auto check_canceled = [&]() -> bool {
        if (shutdown_.load()) return true;
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        return cancel_requested_.count(job_id) > 0;
    };

    try {
        if (check_canceled()) {
            log_to_file("Canceled before initialization started");
            throw std::runtime_error("Canceled during initialization");
        }

        // Resolve SLURM profile and expected job runtime.
        // exp_time = how long the job is expected to run (for allocation matching).
        // time = how long to request from SLURM (allocation duration).
        // If exp_time is not set, fall back to DEFAULT_JOB_TIME (5 min).
        auto profile = allocs_.resolve_profile(job_name);
        const auto& proj = config_.project();
        auto it = proj.jobs.find(job_name);
        std::string exp_time;
        if (it != proj.jobs.end() && !it->second.exp_time.empty()) {
            exp_time = it->second.exp_time;
        } else {
            exp_time = DEFAULT_JOB_TIME;
        }
        int job_minutes = AllocationManager::parse_time_minutes(exp_time);

        // Resolve GPU partition early
        bool needs_gpu = profile.gpu_count > 0 || !profile.gpu_type.empty();
        if (needs_gpu) {
            log_to_file("Resolving GPU partition");
            // Pass empty partition to force discovery unless already a GPU partition
            std::string resolve_part = (profile.partition == "gpu" || profile.partition == "preempt")
                                        ? profile.partition : "";
            auto match = resolve_gpu_partition(login_, username_,
                                               resolve_part,
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
        AllocationState* alloc = allocs_.claim_free(job_minutes, profile, job_id, log_to_file);
        bool claimed_existing = (alloc != nullptr);
        if (alloc) {
            log_to_file(fmt::format("Reusing allocation {} on {}", alloc->slurm_id, alloc->node));
        } else {
            // Check for pending allocation with compatible resources
            AllocationState* pending = allocs_.find_pending(profile);
            if (pending) {
                log_to_file(fmt::format("Found pending allocation {}, waiting for node", pending->slurm_id));
                auto wait_result = allocs_.wait_for_allocation(pending->slurm_id, log_to_file);
                if (wait_result.is_ok()) {
                    alloc = allocs_.find_by_id(wait_result.value.slurm_id);
                }
            }

            if (!alloc) {
                log_to_file("No free allocation, requesting new one");
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

        if (check_canceled()) {
            log_to_file("Canceled after allocation acquired (keeping allocation)");
            throw std::runtime_error("Canceled during initialization");
        }

        // Ensure directories, environment, and SSH keys
        log_to_file("Creating remote directories");
        ensure_dirs(job_id, nullptr);
        if (check_canceled()) throw std::runtime_error("Canceled during initialization");

        log_to_file("Checking environment");
        ensure_environment(alloc->node, log_to_file);
        if (check_canceled()) throw std::runtime_error("Canceled during initialization");

        // Sync code to compute node
        std::string scratch = scratch_dir(job_id);
        log_to_file(fmt::format("Syncing code to {}", alloc->node));
        sync_.sync_to_scratch(alloc->node, scratch, allocs_.state(), log_to_file);

        if (check_canceled()) {
            log_to_file("Canceled after sync");
            throw std::runtime_error("Canceled during initialization");
        }

        // Install requirements into venv (on NFS, via compute node)
        log_to_file("Checking requirements");
        ensure_requirements(alloc->node, scratch, log_to_file);

        if (check_canceled()) {
            throw std::runtime_error("Canceled during initialization");
        }

        // Launch job on compute node
        log_to_file("Launching job on compute node");
        auto launch_result = launch_on_node(job_id, job_name, alloc->node, scratch, extra_args, nullptr);
        if (launch_result.is_err()) {
            throw std::runtime_error(launch_result.error);
        }

        log_to_file(fmt::format("Job launched successfully on {}", alloc->node));

        // Start port forwarding tunnels if configured
        std::vector<std::shared_ptr<TunnelHandle>> tunnel_pids;
        std::vector<int> fwd_ports;
        auto job_it = config_.project().jobs.find(job_name);
        if (job_it != config_.project().jobs.end() && !job_it->second.ports.empty()) {
            fwd_ports = job_it->second.ports;
            tunnel_pids = port_fwd_.start(alloc->node, fwd_ports, log_to_file);
        }

        log_to_file("=== Job running ===");

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
                    tj.tunnel_pids = std::move(tunnel_pids);
                    tj.forwarded_ports = fwd_ports;
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
                js.forwarded_ports = fwd_ports;
                break;
            }
        }
        if (!claimed_existing) {
            allocs_.assign_job(alloc->slurm_id, job_id);
        } else {
            allocs_.persist();
        }

        // New job is running — update watcher targets
        refresh_watcher_targets();

        tccp_log(fmt::format("INIT THREAD COMPLETE: job_id={}", job_id));

        {
            std::lock_guard<std::mutex> lock(cancel_mutex_);
            cancel_requested_.erase(job_id);
        }

    } catch (const std::exception& e) {
        std::string error = fmt::format("Init failed: {}", e.what());
        bool was_canceled = check_canceled();

        log_to_file(error);
        tccp_log(fmt::format("INIT THREAD ERROR: {}", error));

        {
            std::lock_guard<std::mutex> lock(tracked_mutex_);
            for (auto& tj : tracked_) {
                if (tj.job_id == job_id) {
                    if (!was_canceled) {
                        tj.init_error = error;
                    }
                    tj.init_complete = true;
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> cancel_lock(cancel_mutex_);
                cancel_requested_.erase(job_id);
            }
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

// ── Accessors ──────────────────────────────────────────────

const std::vector<TrackedJob>& JobManager::tracked_jobs() const {
    return tracked_;
}

std::vector<TrackedJob> JobManager::tracked_jobs_copy() const {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
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
