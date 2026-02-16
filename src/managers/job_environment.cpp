#include "job_manager.hpp"
#include "job_log.hpp"
#include <environments/environment.hpp>
#include <core/constants.hpp>
#include <fmt/format.h>
#include <chrono>

// ── Directory setup ────────────────────────────────────────

void JobManager::ensure_dirs(const std::string& job_id, StatusCallback cb) {
    std::string base = persistent_base();
    std::string out = job_output_dir(job_id);
    std::string env = env_dir();
    std::string cc = container_cache();

    // Remote directories on cluster
    std::string cmd = fmt::format(
        "mkdir -p {} {}/default/venv {}/images {}/cache {}/tmp {}",
        base, env, cc, cc, cc, out);
    dtn_.run(cmd);

    // Local output directory: output/{job_name}/
    std::filesystem::path local_output = config_.project_dir() / "output" / job_name_from_id(job_id);
    std::error_code ec;
    std::filesystem::create_directories(local_output, ec);
}

void JobManager::ensure_environment(const std::string& compute_node, StatusCallback cb) {
    // Skip check if already validated this session
    if (environment_checked_) {
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() -> std::string {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms < 1000) return fmt::format("{}ms", ms);
        return fmt::format("{:.1f}s", ms / 1000.0);
    };

    // Evict stale cache items if storage exceeds soft cap
    const auto& env_info = get_environment(config_.project().type);
    if (cb) cb(fmt::format("[{}] Checking cluster storage usage...", elapsed()));
    cache_.ensure_within_cap(config_.project().name, env_info.sif_filename, cb);

    if (cb) cb(fmt::format("[{}] Checking environment components...", elapsed()));

    const auto& env = get_environment(config_.project().type);
    std::string cc = container_cache();
    std::string image = cc + "/images/" + env.sif_filename;
    std::string docker_uri = env.docker_uri;
    std::string venv = env_dir() + "/default/venv";
    std::string tccp_home = fmt::format(REMOTE_TCCP_HOME, username_);
    std::string dtach_bin = tccp_home + "/bin/dtach";
    bool gpu = env.gpu;

    // Single check for all three components
    std::string check_all = fmt::format(
        "test -f {} && echo IMAGE_OK || echo IMAGE_MISSING; "
        "( test -f {}/bin/python || test -L {}/bin/python ) && echo VENV_OK || echo VENV_MISSING; "
        "test -x {} && echo DTACH_OK || echo DTACH_MISSING",
        image, venv, venv, dtach_bin);

    auto result = dtn_.run(check_all);
    bool need_image = result.stdout_data.find("IMAGE_MISSING") != std::string::npos;
    bool need_venv = result.stdout_data.find("VENV_MISSING") != std::string::npos;
    bool need_dtach = result.stdout_data.find("DTACH_MISSING") != std::string::npos;

    // Show what we found
    if (cb) {
        if (!need_image) cb(fmt::format("[{}] Container image ({}): found", elapsed(), env.sif_filename));
        else cb(fmt::format("[{}] Container image ({}): MISSING — will pull", elapsed(), env.sif_filename));

        if (!need_venv) cb(fmt::format("[{}] Python venv: found", elapsed()));
        else cb(fmt::format("[{}] Python venv: MISSING — will create", elapsed()));

        if (!need_dtach) cb(fmt::format("[{}] dtach binary: found", elapsed()));
        else cb(fmt::format("[{}] dtach binary: MISSING — will build", elapsed()));
    }

    // Only load modules if we need to create something
    if (need_image || need_venv) {
        if (cb) cb(fmt::format("[{}] Loading Singularity/Apptainer module...", elapsed()));
        dtn_.run("module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true");
        if (cb) cb(fmt::format("[{}] Module loaded", elapsed()));
    }

    // Pull container image if missing — runs on compute node via SSH hop
    // through DTN. Compute node /tmp is large and not quota-limited, unlike
    // DTN /tmp (often full) or home dir (30GB quota).
    if (need_image) {
        if (cb) cb(fmt::format("[{}] Pulling container on {}: {} (this may take 5-15 min)...",
                               elapsed(), compute_node, docker_uri));
        dtn_.run(fmt::format("mkdir -p {}/images", cc));
        std::string pull_cache = fmt::format("/tmp/{}/singularity-cache", username_);
        std::string pull_tmp = fmt::format("/tmp/{}/singularity-tmp", username_);
        std::string pull_cmd = fmt::format(
            "ssh {} {} '"
            "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
            "mkdir -p {} {}; "
            "SINGULARITY_CACHEDIR={} SINGULARITY_TMPDIR={} singularity pull {} {}; "
            "rm -rf {} {}'",
            SSH_OPTS, compute_node,
            pull_cache, pull_tmp,
            pull_cache, pull_tmp, image, docker_uri,
            pull_cache, pull_tmp);
        auto pull_result = dtn_.run(pull_cmd, CONTAINER_PULL_TIMEOUT_SECS);
        if (pull_result.exit_code != 0 && !pull_result.stderr_data.empty()) {
            throw std::runtime_error(fmt::format("Container pull failed ({}): {}",
                                                 pull_result.stderr_data, pull_result.get_output()));
        }
        if (cb) cb(fmt::format("[{}] Container pull finished, verifying...", elapsed()));
        // Verify the image was actually pulled (check from DTN — NFS-visible)
        auto img_check = dtn_.run("test -f " + image + " && echo IMG_OK || echo IMG_FAIL");
        if (img_check.stdout_data.find("IMG_FAIL") != std::string::npos) {
            throw std::runtime_error(fmt::format("Container pull failed: {}", pull_result.get_output()));
        }
        // Report image size
        auto sz = dtn_.run(fmt::format("du -sh {} 2>/dev/null | cut -f1", image));
        std::string size_str = sz.stdout_data;
        while (!size_str.empty() && (size_str.back() == '\n' || size_str.back() == '\r'))
            size_str.pop_back();
        if (cb) cb(fmt::format("[{}] Container image ready ({})", elapsed(), size_str.empty() ? "?" : size_str));
    }

    // Create venv if missing
    if (need_venv) {
        if (cb) cb(fmt::format("[{}] Creating Python venv ({}site-packages)...",
                               elapsed(), gpu ? "with system " : ""));
        std::string base = persistent_base();
        std::string venv_flags = gpu ? "--system-site-packages " : "";
        // Do NOT use --nv for venv creation: it runs on the DTN which has no
        // GPU driver, so singularity exec --nv would fail silently.
        // CUDA is not needed just to create a venv.
        std::string pip_tmp = fmt::format("/tmp/{}/tccp-pip-tmp", username_);
        dtn_.run(fmt::format("mkdir -p {}", pip_tmp));
        std::string venv_cmd = fmt::format(
            "TMPDIR={} singularity exec --bind {}:{} {} python -m venv {}{}",
            pip_tmp, base, base, image, venv_flags, venv);
        auto venv_result = dtn_.run(venv_cmd);
        if (venv_result.exit_code != 0) {
            throw std::runtime_error(fmt::format(
                "Failed to create venv: {}", venv_result.get_output()));
        }
        dtn_.run(fmt::format("rm -rf {} 2>/dev/null; true", pip_tmp));
        if (cb) cb(fmt::format("[{}] Python venv created", elapsed()));
    }

    // Build dtach if missing
    if (need_dtach) {
        if (cb) cb(fmt::format("[{}] Building dtach...", elapsed()));
        ensure_dtach(cb);
        if (cb) cb(fmt::format("[{}] dtach ready", elapsed()));
    }

    // Mark as checked for this session
    environment_checked_ = true;

    // Touch LRU timestamps on the container and env we just validated
    cache_.touch_used(image, env_dir());

    if (cb) cb(fmt::format("[{}] Environment ready", elapsed()));
}

void JobManager::ensure_dtach(StatusCallback cb) {
    std::string tccp_home = fmt::format(REMOTE_TCCP_HOME, username_);
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

    if (cb) cb("Downloading dtach source...");

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

    if (cb) cb("Compiling dtach...");
    auto build = dtn_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1");
    if (build.exit_code != 0) {
        if (cb) cb("Direct compile failed, trying configure/make...");
        auto build2 = dtn_.run("cd " + build_dir + " && ./configure && make 2>&1");
        if (build2.exit_code != 0) {
            throw std::runtime_error(fmt::format(
                "Failed to compile dtach: {}", build2.get_output()));
        }
    }

    if (cb) cb("Installing dtach binary...");
    dtn_.run("cp " + build_dir + "/dtach " + bin + " && chmod +x " + bin);
    dtn_.run("rm -rf " + build_dir);

    auto verify = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_FAIL");
    if (verify.stdout_data.find("DTACH_OK") != std::string::npos) {
        if (cb) cb("dtach compiled and installed");
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
    std::string out_dir = job_output_dir(job_id);
    std::string sock = "/tmp/tccp_" + job_id + ".sock";

    if (cb) cb("Launching job on compute node...");

    // Resolve all environment paths and build the run script
    auto paths = resolve_env_paths(job_id, scratch);
    std::string run_script = build_run_preamble(paths, scratch)
                           + build_job_payload(job_name, paths);

    // Write to DTN first, then scp to compute node
    std::string dtn_script = fmt::format("/tmp/tccp_run_{}.sh", job_id);
    std::string heredoc_cmd = "cat > " + dtn_script + " << 'TCCP_RUN_EOF'\n"
                              + run_script + "\nTCCP_RUN_EOF";
    auto r = dtn_.run(heredoc_cmd);
    tccp_log_ssh("launch:write-script", "cat > " + dtn_script, r);

    dtn_.run("chmod +x " + dtn_script);

    // Copy script to compute node
    r = dtn_.run(fmt::format("scp {} {} {}:{}/tccp_run.sh",
                             SSH_OPTS, dtn_script, compute_node, scratch));
    tccp_log_ssh("launch:scp-script", "scp script to node", r);
    dtn_.run("rm -f " + dtn_script);

    // Create output symlink
    r = dtn_.run(fmt::format("ssh {} {} 'ln -sfn {} {}/output'",
                             SSH_OPTS, compute_node, out_dir, scratch));
    tccp_log_ssh("launch:output-symlink", "ln -sfn", r);

    // Create shared cache directory
    if (!proj.cache.empty()) {
        std::string shared_cache = fmt::format("/tmp/{}/{}/.tccp-cache",
                                               username_, proj.name);
        r = dtn_.run(fmt::format("ssh {} {} 'mkdir -p {}'",
                                 SSH_OPTS, compute_node, shared_cache));
        tccp_log_ssh("launch:cache-dir", "mkdir cache", r);
    }

    // Launch dtach
    std::string launch_cmd = fmt::format(
        "ssh {} {} '$HOME/tccp/bin/dtach -n {} {}/tccp_run.sh'",
        SSH_OPTS, compute_node, sock, scratch);
    r = dtn_.run(launch_cmd);
    tccp_log_ssh("launch:dtach", launch_cmd, r);

    if (r.exit_code != 0) {
        return Result<void>::Err(fmt::format("Failed to launch dtach: {}", r.get_output()));
    }

    if (cb) cb(fmt::format("Job launched on {}", compute_node));
    return Result<void>::Ok();
}
