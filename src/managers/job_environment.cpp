#include "job_manager.hpp"
#include "job_log.hpp"
#include <environments/environment.hpp>
#include <core/constants.hpp>
#include <fmt/format.h>

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

    const auto& env = get_environment(config_.project().type);
    std::string cc = container_cache();
    std::string image = cc + "/images/" + env.sif_filename;
    std::string venv = env_dir() + "/default/venv";
    std::string out_dir = job_output_dir(job_id);
    std::string sock = "/tmp/tccp_" + job_id + ".sock";
    std::string log = "/tmp/tccp_" + job_id + ".log";
    bool gpu = env.gpu;
    std::string nv_flag = gpu ? "--nv " : "";

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
