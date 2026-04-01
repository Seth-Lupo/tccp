#include "job_manager.hpp"
#include "job_log.hpp"
#include <environments/environment.hpp>
#include <core/constants.hpp>
#include <ssh/compute_hop.hpp>
#include <fmt/format.h>
#include <chrono>

// ── Directory setup ────────────────────────────────────────

void JobManager::ensure_dirs(const std::string& job_id, StatusCallback cb) {
    std::string cc = container_cache();

    // Remote directories on cluster (only container cache on NFS)
    std::string cmd = fmt::format(
        "mkdir -p {}/images {}/cache {}/tmp",
        cc, cc, cc);
    dtn_.run(cmd);

    // Local output directory: output/{job_name}/
    std::filesystem::path local_output = config_.project_dir() / "output" / job_name_from_id(job_id);
    std::error_code ec;
    std::filesystem::create_directories(local_output, ec);
}

void JobManager::ensure_container(const std::string& compute_node,
                                   const EnvironmentConfig& env,
                                   const std::string& image,
                                   StatusCallback cb) {
    std::string cc = container_cache();
    std::string docker_uri = env.docker_uri;

    if (cb) cb(fmt::format("Pulling container on {}: {} (this may take 5-15 min)...",
                           compute_node, docker_uri));
    dtn_.run(fmt::format("mkdir -p {}/images", cc));
    std::string pull_cache = fmt::format("/tmp/{}/singularity-cache", username_);
    std::string pull_tmp = fmt::format("/tmp/{}/singularity-tmp", username_);
    ComputeHop compute(dtn_, compute_node);
    auto pull_result = compute.run(fmt::format(
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
        "mkdir -p {} {}; "
        "SINGULARITY_CACHEDIR={} SINGULARITY_TMPDIR={} singularity pull {} {}; "
        "rm -rf {} {}",
        pull_cache, pull_tmp,
        pull_cache, pull_tmp, image, docker_uri,
        pull_cache, pull_tmp), CONTAINER_PULL_TIMEOUT_SECS);
    if (pull_result.exit_code != 0) {
        throw std::runtime_error(fmt::format("Container pull failed ({}): {}",
                                             pull_result.stderr_data, pull_result.get_output()));
    }
    if (cb) cb("Container pull finished, verifying...");
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
    if (cb) cb(fmt::format("Container image ready ({})", size_str.empty() ? "?" : size_str));
}

void JobManager::ensure_venv(const std::string& compute_node,
                              const EnvironmentConfig& env,
                              const std::string& image,
                              const std::string& venv,
                              StatusCallback cb) {
    if (cb) cb(fmt::format("Creating Python venv ({}site-packages)...",
                           env.gpu ? "with system " : ""));
    std::string venv_flags = env.gpu ? "--system-site-packages " : "";
    std::string pip_tmp = fmt::format("/tmp/{}/tccp-pip-tmp", username_);
    // Create venv on compute node (venv is on compute /tmp now)
    ComputeHop compute(dtn_, compute_node);
    auto venv_result = compute.run(fmt::format(
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
        "mkdir -p {} {}; "
        "TMPDIR={} singularity exec {} python -m venv {}{}; "
        "rm -rf {} 2>/dev/null; true",
        pip_tmp, venv,
        pip_tmp, image, venv_flags, venv,
        pip_tmp), 120);
    if (venv_result.exit_code != 0) {
        throw std::runtime_error(fmt::format(
            "Failed to create venv: {}", venv_result.get_output()));
    }
    if (cb) cb("Python venv created");
}

void JobManager::ensure_environment(const std::string& compute_node, StatusCallback cb) {
    if (environment_checked_node_ == compute_node) {
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&]() -> std::string {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (ms < 1000) return fmt::format("{}ms", ms);
        return fmt::format("{:.1f}s", ms / 1000.0);
    };

    if (cb) cb(fmt::format("[{}] Checking environment components...", elapsed()));

    const auto& env = get_environment(config_.project().type);
    std::string cc = container_cache();
    std::string image = cc + "/images/" + env.sif_filename;
    std::string venv = env_dir() + "/default/venv";
    std::string tccp_home = fmt::format(REMOTE_TCCP_HOME, username_);
    std::string dtach_bin = tccp_home + "/bin/dtach";

    std::string uv_bin = tccp_home + "/bin/uv";

    // Check NFS components (image, dtach, uv) from DTN
    std::string check_nfs = fmt::format(
        "test -f {} && echo IMAGE_OK || echo IMAGE_MISSING; "
        "test -x {} && echo DTACH_OK || echo DTACH_MISSING; "
        "test -x {} && echo UV_OK || echo UV_MISSING",
        image, dtach_bin, uv_bin);
    auto result = dtn_.run(check_nfs);

    // Check venv on compute node (it's on /tmp now)
    ComputeHop compute(dtn_, compute_node);
    auto venv_check = compute.run(fmt::format(
        "( test -f {}/bin/python || test -L {}/bin/python ) && echo VENV_OK || echo VENV_MISSING",
        venv, venv));
    // Merge results
    result.stdout_data += venv_check.stdout_data;
    bool need_image = result.stdout_data.find("IMAGE_MISSING") != std::string::npos;
    bool need_venv = result.stdout_data.find("VENV_MISSING") != std::string::npos;
    bool need_dtach = result.stdout_data.find("DTACH_MISSING") != std::string::npos;
    bool need_uv = result.stdout_data.find("UV_MISSING") != std::string::npos;

    if (cb) {
        if (!need_image) cb(fmt::format("[{}] Container image ({}): found", elapsed(), env.sif_filename));
        else cb(fmt::format("[{}] Container image ({}): MISSING — will pull", elapsed(), env.sif_filename));

        if (!need_venv) cb(fmt::format("[{}] Python venv: found", elapsed()));
        else cb(fmt::format("[{}] Python venv: MISSING — will create", elapsed()));

        if (!need_dtach) cb(fmt::format("[{}] dtach binary: found", elapsed()));
        else cb(fmt::format("[{}] dtach binary: MISSING — will build", elapsed()));

        if (!need_uv) cb(fmt::format("[{}] uv: found", elapsed()));
        else cb(fmt::format("[{}] uv: MISSING — will install", elapsed()));
    }

    if (need_image) {
        if (cb) cb(fmt::format("[{}] Loading Singularity/Apptainer module...", elapsed()));
        dtn_.run("module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true", 30);
        if (cb) cb(fmt::format("[{}] Module loaded", elapsed()));
    }

    if (need_image) {
        ensure_container(compute_node, env, image, cb);
    }

    if (need_venv) {
        ensure_venv(compute_node, env, image, venv, cb);
    }

    if (need_dtach) {
        if (cb) cb(fmt::format("[{}] Building dtach...", elapsed()));
        ensure_dtach(cb);
        if (cb) cb(fmt::format("[{}] dtach ready", elapsed()));
    }

    if (need_uv) {
        if (cb) cb(fmt::format("[{}] Installing uv...", elapsed()));
        ensure_uv(cb);
        if (cb) cb(fmt::format("[{}] uv ready", elapsed()));
    }

    environment_checked_node_ = compute_node;
    cache_.touch_used(image);

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

    auto clone = dtn_.run("git clone https://github.com/crigler/dtach.git " + build_dir + " 2>&1", 60);
    if (clone.exit_code != 0) {
        // git might not be available — try curl
        if (cb) cb("git not available, trying curl...");
        dtn_.run("mkdir -p " + build_dir);
        auto dl = dtn_.run("curl -sL https://github.com/crigler/dtach/archive/refs/heads/master.tar.gz "
                           "| tar xz -C " + build_dir + " --strip-components=1 2>&1", 60);
        if (dl.exit_code != 0) {
            throw std::runtime_error(fmt::format(
                "Cannot download dtach source (no git or curl?): {}", dl.get_output()));
        }
    }

    if (cb) cb("Compiling dtach...");
    auto build = dtn_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1", 60);
    if (build.exit_code != 0) {
        if (cb) cb("Direct compile failed, trying configure/make...");
        auto build2 = dtn_.run("cd " + build_dir + " && ./configure && make 2>&1", 120);
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

void JobManager::ensure_uv(StatusCallback cb) {
    std::string tccp_home = fmt::format(REMOTE_TCCP_HOME, username_);
    std::string bin = tccp_home + "/bin/uv";

    auto check = dtn_.run("test -x " + bin + " && echo UV_OK || echo UV_MISSING");
    if (check.stdout_data.find("UV_OK") != std::string::npos) {
        return;
    }

    dtn_.run("mkdir -p " + tccp_home + "/bin");

    // Download uv static binary (single file, no dependencies)
    if (cb) cb("Downloading uv...");
    auto dl = dtn_.run(fmt::format(
        "curl -fsSL https://github.com/astral-sh/uv/releases/latest/download/"
        "uv-x86_64-unknown-linux-gnu.tar.gz | tar xz --strip-components=1 -C {} "
        "uv-x86_64-unknown-linux-gnu/uv 2>&1",
        tccp_home + "/bin"), 120);

    if (dl.exit_code != 0) {
        // Fallback: try the install script
        if (cb) cb("Direct download failed, trying install script...");
        dl = dtn_.run(fmt::format(
            "curl -LsSf https://astral.sh/uv/install.sh | "
            "UV_INSTALL_DIR={} sh 2>&1",
            tccp_home + "/bin"), 120);
    }

    auto verify = dtn_.run("test -x " + bin + " && echo UV_OK || echo UV_FAIL");
    if (verify.stdout_data.find("UV_OK") != std::string::npos) {
        if (cb) cb("uv installed");
    } else {
        // Non-fatal: fall back to pip if uv can't be installed
        if (cb) cb("Warning: could not install uv, will use pip instead");
    }
}

// ── Requirements ──────────────────────────────────────────

void JobManager::ensure_requirements(const std::string& compute_node,
                                      const std::string& scratch,
                                      StatusCallback cb) {
    const auto& env = get_environment(config_.project().type);
    std::string venv = env_dir() + "/default/venv";
    std::string cc = container_cache();
    std::string image = cc + "/images/" + env.sif_filename;
    std::string nv_flag = env.gpu ? "--nv " : "";
    std::string stamp_path = venv + "/.req_installed";
    std::string pip_tmp = fmt::format("/tmp/{}/tccp-pip-tmp", username_);

    // Check if requirements.txt exists on compute node (scratch is on /tmp, not NFS)
    ComputeHop compute(dtn_, compute_node);
    auto check = compute.run(fmt::format(
        "test -f {}/requirements.txt "
        "&& md5sum {}/requirements.txt | cut -d' ' -f1 "
        "|| echo NO_REQ", scratch, scratch));

    std::string req_hash = check.stdout_data;
    while (!req_hash.empty() && (req_hash.back() == '\n' || req_hash.back() == '\r'))
        req_hash.pop_back();

    if (req_hash == "NO_REQ" || req_hash.empty()) {
        return;
    }

    // Stamp is on compute node /tmp — read via SSH hop.
    // Format: "v2:<md5>" to version-gate against stale stamps.
    auto stamp_result = compute.run(fmt::format(
        "cat {} 2>/dev/null || echo NO_STAMP", stamp_path));
    std::string stamp = stamp_result.stdout_data;
    while (!stamp.empty() && (stamp.back() == '\n' || stamp.back() == '\r'))
        stamp.pop_back();

    std::string expected_stamp = "v2:" + req_hash;
    if (stamp == expected_stamp) {
        if (cb) cb("Requirements already installed");
        return;
    }

    if (cb) cb("Installing requirements...");

    // GPU environments: filter out torch/nvidia packages (container provides them)
    std::string req_file = scratch + "/requirements.txt";
    if (env.gpu) {
        std::string filtered = scratch + "/.tccp-tmp/tccp_req_filtered.txt";
        // NOTE: no single quotes around grep pattern — ComputeHop wraps
        // the entire command in single quotes already. Use double quotes.
        compute.run(fmt::format(
            "mkdir -p {scratch}/.tccp-tmp; "
            "grep -ivE "
            "\"^(torch|torchvision|torchaudio|pytorch|nvidia-|triton)([[:space:]]|[=><!~\\\\[]|$)\" "
            "{req} > {filt} || true",
            fmt::arg("scratch", scratch),
            fmt::arg("req", req_file),
            fmt::arg("filt", filtered)));
        req_file = filtered;

        // Create stub .dist-info dirs in the venv for container-provided packages.
        // This makes uv/pip see torch, nvidia, triton, etc. as already installed
        // so they're skipped entirely — no download, no disk usage.
        // Write the Python script to a temp file on DTN, scp to compute node,
        // and execute there — avoids single-quote nesting issues with ComputeHop.
        std::string stub_script = fmt::format(
            "import importlib.metadata, pathlib\n"
            "pkgs = [(d.name, d.version) for d in importlib.metadata.distributions()\n"
            "        if any(d.name.lower().startswith(p) for p in\n"
            "        ['torch', 'nvidia-', 'triton', 'cuda-', 'sympy', 'numpy'])]\n"
            "sp = next(pathlib.Path('{venv}/lib').glob('python*/site-packages'))\n"
            "for name, ver in pkgs:\n"
            "    di = sp / f'{{name.replace(chr(45), chr(95))}}-{{ver}}.dist-info'\n"
            "    if di.exists(): continue\n"
            "    di.mkdir(parents=True, exist_ok=True)\n"
            "    (di / 'METADATA').write_text(f'Metadata-Version: 2.1\\nName: {{name}}\\nVersion: {{ver}}\\n')\n"
            "    (di / 'INSTALLER').write_text('tccp-stub')\n"
            "    (di / 'RECORD').write_text('')\n",
            fmt::arg("venv", venv));

        std::string dtn_script = fmt::format("/tmp/{}/tccp_stubs.py", username_);
        dtn_.run("cat > " + dtn_script + " << 'TCCP_STUBS_EOF'\n"
                 + stub_script + "\nTCCP_STUBS_EOF");
        std::string node_script = scratch + "/.tccp-tmp/tccp_stubs.py";
        dtn_.run(fmt::format("scp {} {} {}:{}", SSH_OPTS, dtn_script, compute_node, node_script));
        dtn_.run("rm -f " + dtn_script);

        compute.run(fmt::format(
            "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
            "singularity exec {} python3 {} 2>/dev/null; rm -f {}",
            image, node_script, node_script), 60);
    }

    // Determine installer: uv (fast, on NFS) or pip (fallback, in container)
    std::string tccp_home = fmt::format(REMOTE_TCCP_HOME, username_);
    std::string uv_bin = tccp_home + "/bin/uv";
    auto uv_check = dtn_.run("test -x " + uv_bin + " && echo UV_OK || echo UV_MISSING");
    bool have_uv = uv_check.stdout_data.find("UV_OK") != std::string::npos;

    // Build install command — runs on compute node via SSH hop.
    // Venv and requirements are both on compute /tmp. Image is on NFS (visible from compute).
    std::string full_cmd;
    if (have_uv) {
        full_cmd = fmt::format(
            "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
            "mkdir -p {tmp}; "
            "TMPDIR={tmp} singularity exec {nv}--bind {venv}:{venv} {image} "
            "{uv} pip install --no-cache --link-mode=copy --python {venv}/bin/python "
            "-r {req} 2>&1",
            fmt::arg("tmp", pip_tmp),
            fmt::arg("nv", nv_flag),
            fmt::arg("uv", uv_bin),
            fmt::arg("venv", venv),
            fmt::arg("image", image),
            fmt::arg("req", req_file));
    } else {
        full_cmd = fmt::format(
            "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
            "mkdir -p {tmp}; "
            "TMPDIR={tmp} PIP_CACHE_DIR={tmp} "
            "singularity exec {nv}--bind {venv}:{venv} {image} "
            "{venv}/bin/pip install -q -r {req} 2>&1",
            fmt::arg("tmp", pip_tmp),
            fmt::arg("nv", nv_flag),
            fmt::arg("venv", venv),
            fmt::arg("image", image),
            fmt::arg("req", req_file));
    }

    if (cb) cb(fmt::format("Installing requirements ({})...", have_uv ? "uv" : "pip"));
    if (cb) cb(fmt::format("[req] cmd: {}", full_cmd));

    auto result = compute.run(full_cmd, 600);

    // Always log exit code + output for robust debugging
    if (cb) {
        cb(fmt::format("[req] exit={}", result.exit_code));
        std::string out = result.stdout_data;
        if (!out.empty()) {
            if (out.size() > 2000) out = "..." + out.substr(out.size() - 2000);
            cb(fmt::format("[req] {}", out));
        }
    }

    if (result.exit_code != 0) {
        std::string output = result.get_output();
        if (output.size() > 500) output = "..." + output.substr(output.size() - 500);
        throw std::runtime_error(fmt::format(
            "Package install failed (exit {}):\n{}", result.exit_code, output));
    }

    // Write versioned stamp on success (on compute node /tmp)
    compute.run(fmt::format("echo 'v2:{}' > {}", req_hash, stamp_path));
    if (cb) cb("Requirements installed");
}

// ── Launch job on compute node ─────────────────────────────

Result<void> JobManager::launch_on_node(const std::string& job_id,
                                         const std::string& job_name,
                                         const std::string& compute_node,
                                         const std::string& scratch,
                                         const std::string& extra_args,
                                         StatusCallback cb) {
    const auto& proj = config_.project();
    std::string sock = scratch + "/tccp.sock";

    if (cb) cb("Launching job on compute node...");

    // Resolve all environment paths and build the run script
    auto paths = resolve_env_paths(job_id, scratch);
    std::string run_script = build_run_preamble(paths, scratch)
                           + build_job_payload(job_name, paths, extra_args);

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

    ComputeHop compute(dtn_, compute_node);

    // Create output directory on scratch (no NFS symlink)
    r = compute.run(fmt::format("mkdir -p {}/output", scratch));
    tccp_log_ssh("launch:output-mkdir", "mkdir -p output", r);

    // Create shared cache directory
    if (!proj.cache.empty()) {
        std::string shared_cache = fmt::format("/tmp/{}/{}/.tccp-cache",
                                               username_, proj.name);
        r = compute.run(fmt::format("mkdir -p {}", shared_cache));
        tccp_log_ssh("launch:cache-dir", "mkdir cache", r);
    }

    // Remove stale socket and log from previous run (if scratch was reused)
    compute.run(fmt::format("rm -f {} {}", sock, paths.log));

    // Launch dtach
    std::string launch_cmd_str = fmt::format("$HOME/tccp/bin/dtach -n {} {}/tccp_run.sh", sock, scratch);
    r = compute.run(launch_cmd_str);
    tccp_log_ssh("launch:dtach", launch_cmd_str, r);

    if (r.exit_code != 0) {
        return Result<void>::Err(fmt::format("Failed to launch dtach: {}", r.get_output()));
    }

    if (cb) cb(fmt::format("Job launched on {}", compute_node));
    return Result<void>::Ok();
}
