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

namespace fs = std::filesystem;

// ── Debug log ──────────────────────────────────────────────
// Always appends to /tmp/tccp_debug.log so it survives across runs.
// Read this file to understand what happened in the latest session.
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

JobManager::JobManager(const Config& config, SSHConnection& dtn, SSHConnection& login)
    : config_(config), dtn_(dtn), login_(login), username_(get_cluster_username()) {
}

std::string JobManager::persistent_base() const {
    return fmt::format("/cluster/home/{}/tccp/{}",
                       username_, config_.project().name);
}

std::string JobManager::job_dir(const std::string& job_id) const {
    return persistent_base() + "/" + job_id;
}

std::string JobManager::generate_job_id(const std::string& job_name) const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);

    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}-{:02d}-{:02d}-{:03d}__{}"  ,
                       tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                       tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                       static_cast<int>(ms.count()), job_name);
}

void JobManager::ensure_dirs(const std::string& job_id, StatusCallback cb) {
    std::string base = persistent_base();
    std::string jd = job_dir(job_id);

    std::string cmd = fmt::format(
        "mkdir -p {}/env/default/venv "
        "{}/container-cache/images "
        "{}/container-cache/cache "
        "{}/container-cache/tmp "
        "{}/output",
        base, base, base, base, jd);

    dtn_.run(cmd);
}

void JobManager::ensure_environment(StatusCallback cb) {
    if (cb) cb("Checking environment...");

    std::string base = persistent_base();
    std::string image = base + "/container-cache/images/python_3.11-slim.sif";
    std::string venv = base + "/env/default/venv";

    // Ensure singularity/apptainer is available on DTN
    dtn_.run("module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true");

    // Pull container image if missing
    std::string check_image = fmt::format("test -f {} && echo EXISTS || echo MISSING", image);
    auto result = dtn_.run(check_image);
    if (result.stdout_data.find("MISSING") != std::string::npos) {
        if (cb) cb("Pulling Python container image (first time only)...");
        std::string pull_cmd = fmt::format(
            "SINGULARITY_CACHEDIR={0}/container-cache/cache "
            "SINGULARITY_TMPDIR={0}/container-cache/tmp "
            "singularity pull --dir {0}/container-cache/images "
            "docker://python:3.11-slim", base);
        dtn_.run(pull_cmd);
    }

    // Create venv if missing (check for bin/python, not just the dir)
    std::string check_venv = fmt::format("test -f {}/bin/python && echo EXISTS || echo MISSING", venv);
    result = dtn_.run(check_venv);
    if (result.stdout_data.find("MISSING") != std::string::npos) {
        if (cb) cb("Creating Python virtual environment...");
        // Need --bind so singularity can write to the cluster filesystem
        std::string venv_cmd = fmt::format(
            "singularity exec --bind {0}:{0} {1} python -m venv {2}",
            base, image, venv);
        dtn_.run(venv_cmd);
    }

    // Build dtach if missing
    ensure_dtach(cb);

    if (cb) cb("Environment OK");
}

void JobManager::ensure_dtach(StatusCallback cb) {
    std::string tccp_home = fmt::format("/cluster/home/{}/tccp", username_);
    std::string bin = tccp_home + "/bin/dtach";

    // Check if dtach binary already exists and works
    auto check = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_MISSING");
    if (check.stdout_data.find("DTACH_OK") != std::string::npos) {
        return;
    }

    if (cb) cb("Building dtach (first time only)...");

    std::string build_dir = tccp_home + "/dtach-build";
    dtn_.run("mkdir -p " + tccp_home + "/bin");
    dtn_.run("rm -rf " + build_dir);
    dtn_.run("git clone https://github.com/crigler/dtach.git " + build_dir);

    // Build dtach (try direct cc first, fall back to configure/make)
    auto build = dtn_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1");
    if (build.exit_code != 0) {
        dtn_.run("cd " + build_dir + " && ./configure && make 2>&1");
    }

    // Install binary
    dtn_.run("cp " + build_dir + "/dtach " + bin + " && chmod +x " + bin);

    // Verify
    auto verify = dtn_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_FAIL");
    if (verify.stdout_data.find("DTACH_OK") == std::string::npos) {
        if (cb) cb("Warning: dtach build may have failed");
    } else {
        if (cb) cb("dtach built successfully");
    }

    // Clean up build dir
    dtn_.run("rm -rf " + build_dir);
}

std::string JobManager::generate_slurm_script(const std::string& job_id,
                                                const std::string& job_name) const {
    std::string base = persistent_base();
    std::string jd = job_dir(job_id);
    const auto& proj = config_.project();
    const auto& user = username_;

    // Look up the job config
    auto it = proj.jobs.find(job_name);
    std::string script = (it != proj.jobs.end()) ? it->second.script : "main.py";
    std::string args = (it != proj.jobs.end()) ? it->second.args : "";

    std::string s;
    s += "#!/bin/bash\n";
    s += fmt::format("#SBATCH --job-name={}\n", job_name);
    s += "#SBATCH --mem=1G\n";
    s += "#SBATCH --time=0:10:00\n";
    s += "#SBATCH --nodes=1\n";
    s += "#SBATCH --cpus-per-task=1\n";
    s += fmt::format("#SBATCH --output={}/slurm-%j.out\n", jd);
    s += fmt::format("#SBATCH --error={}/slurm-%j.err\n", jd);
    s += "\n";

    s += fmt::format("SCRATCH=\"/tmp/{}/{}/{}\"\n", user, proj.name, job_id);
    s += fmt::format("OUTPUT=\"{}/output\"\n", jd);
    s += fmt::format("IMAGE=\"{}/container-cache/images/python_3.11-slim.sif\"\n", base);
    s += fmt::format("VENV=\"{}/env/default/venv\"\n", base);
    s += fmt::format("CACHE=\"{}/container-cache\"\n", base);
    s += "\n";

    // Debug log on the cluster side
    s += fmt::format("TCCP_LOG=\"{}/tccp_debug.log\"\n", jd);
    s += "tccp_dbg() { echo \"[$(date +%H:%M:%S)] $*\" >> \"$TCCP_LOG\"; }\n";
    s += "\n";

    s += "tccp_dbg \"=== SLURM script start ===\"\n";
    s += "tccp_dbg \"SCRATCH=$SCRATCH\"\n";
    s += "tccp_dbg \"NODE=$(hostname)\"\n";
    s += "tccp_dbg \"SLURM_JOB_ID=$SLURM_JOB_ID\"\n";
    s += "\n";

    // Load modules
    s += "# ── Modules ──\n";
    s += "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true\n";
    s += "\n";

    // Wait for post-allocation staging from tccp client
    s += "# ── Wait for code staging (post-allocation) ──\n";
    s += "mkdir -p \"$SCRATCH\"\n";
    s += "tccp_dbg \"Waiting for .tccp_ready...\"\n";
    s += "while [ ! -f \"$SCRATCH/.tccp_ready\" ]; do sleep 1; done\n";
    s += "tccp_dbg \"Ready signal received\"\n";
    s += "tccp_dbg \"SCRATCH contents: $(ls -la $SCRATCH 2>&1)\"\n";
    s += "\n";

    // Deps
    s += "# ── Deps (idempotent) ──\n";
    s += "export SINGULARITY_CACHEDIR=\"$CACHE/cache\"\n";
    s += "export SINGULARITY_TMPDIR=\"$CACHE/tmp\"\n";
    s += "if [ -f \"$SCRATCH/requirements.txt\" ]; then\n";
    s += "    singularity exec --bind \"$SCRATCH\":\"$SCRATCH\" --bind \"$VENV\":\"$VENV\" \"$IMAGE\" \"$VENV/bin/pip\" install -q -r \"$SCRATCH/requirements.txt\"\n";
    s += "fi\n";
    s += "\n";

    // dtach session
    s += "# ── dtach session ──\n";
    s += "cd \"$SCRATCH\"\n";
    s += "DTACH=\"$HOME/tccp/bin/dtach\"\n";
    s += "SOCK=\"/tmp/tccp_${SLURM_JOB_ID}.sock\"\n";
    s += "\n";

    // Session log for output replay on reattach
    s += "LOG=\"/tmp/tccp_${SLURM_JOB_ID}.log\"\n";
    s += "\n";

    s += "tccp_dbg \"DTACH=$DTACH SOCK=$SOCK\"\n";
    s += "tccp_dbg \"test dtach: $(test -x $DTACH && echo OK || echo MISSING)\"\n";
    s += "\n";

    // Write the inner run script (avoids quoting issues with bash -c)
    s += "cat > \"$SCRATCH/tccp_run.sh\" << 'TCCP_RUN_EOF'\n";
    s += "#!/bin/bash\n";
    s += fmt::format(
        "CMD=\"singularity exec --bind $SCRATCH:$SCRATCH $IMAGE $VENV/bin/python {} {}\"\n",
        script, args);
    s += "script -fq -c \"$CMD\" \"$LOG\"\n";
    s += "EC=$?\n";
    s += "printf '\\033[J'\n";  // clear remaining PTY blank lines
    s += "TCCP_RUN_EOF\n";
    s += "chmod +x \"$SCRATCH/tccp_run.sh\"\n";
    s += "\n";

    // Export vars so the inner script can use them
    s += "export SCRATCH IMAGE VENV LOG\n";
    s += "\n";

    s += "tccp_dbg \"Launching dtach...\"\n";

    // Launch dtach in detached mode (-n = create, don't attach)
    s += "\"$DTACH\" -n \"$SOCK\" \"$SCRATCH/tccp_run.sh\"\n";
    s += "tccp_dbg \"dtach launch exit=$?\"\n";
    s += "\n";

    s += "tccp_dbg \"Waiting for dtach socket to disappear...\"\n";

    // Wait for dtach socket to disappear (process ended)
    s += "# Wait for dtach session to end\n";
    s += "while [ -e \"$SOCK\" ]; do\n";
    s += "    sleep 5\n";
    s += "done\n";
    s += "\n";

    s += "tccp_dbg \"Session ended\"\n";

    // Cleanup — remove scratch, keep only output/logs/env
    s += "# ── Cleanup ──\n";
    s += "rm -rf \"$SCRATCH\"\n";
    s += "tccp_dbg \"=== SLURM script done ===\"\n";

    return s;
}

void JobManager::stage_to_node(const std::string& job_id,
                               const std::string& compute_node,
                               StatusCallback cb) {
    tccp_log(fmt::format("=== stage_to_node job_id={} node={} ===", job_id, compute_node));

    // Ensure environment (container, venv, dtach) before staging
    ensure_environment(cb);

    const auto& proj = config_.project();
    std::string scratch = fmt::format("/tmp/{}/{}/{}", username_, proj.name, job_id);
    std::string dtn_tmp = fmt::format("/tmp/tccp_stage_{}", job_id);
    std::string jd = job_dir(job_id);

    tccp_log(fmt::format("scratch={} dtn_tmp={} jd={}", scratch, dtn_tmp, jd));

    // ── Collect code files (reuse GitignoreParser, filter out rodata) ──
    GitignoreParser parser(config_.project_dir());
    auto files = parser.collect_files();

    std::vector<fs::path> code_files;
    for (const auto& f : files) {
        auto rel = parser.get_relative_path(f);
        std::string rel_str = rel.string();
        if (rel_str.substr(0, 7) != "rodata/" && rel_str != "rodata") {
            code_files.push_back(f);
        }
    }

    if (cb) cb(fmt::format("Staging code ({} files) to compute node...", code_files.size()));
    tccp_log(fmt::format("code_files count={}", code_files.size()));

    // ── Upload code to DTN temp dir ──
    std::string code_tmp = dtn_tmp + "/code";
    std::set<std::string> dirs;
    for (const auto& f : code_files) {
        auto rel = parser.get_relative_path(f);
        auto parent = rel.parent_path();
        if (!parent.empty() && parent != ".") {
            dirs.insert(code_tmp + "/" + parent.string());
        }
    }
    // Create all needed dirs on DTN
    std::string mkdir_cmd = "mkdir -p " + code_tmp;
    for (const auto& d : dirs) {
        mkdir_cmd += " " + d;
    }
    auto r = dtn_.run(mkdir_cmd);
    tccp_log_ssh("stage:mkdirs", mkdir_cmd, r);

    for (const auto& f : code_files) {
        auto rel = parser.get_relative_path(f);
        std::string remote = code_tmp + "/" + rel.string();
        dtn_.upload(f, remote);
    }
    tccp_log(fmt::format("uploaded {} code files to DTN {}", code_files.size(), code_tmp));

    // ── Upload rodata to DTN temp dir ──
    const auto& rodata = proj.rodata;
    if (!rodata.empty()) {
        if (cb) cb(fmt::format("Staging rodata ({} dirs) to compute node...", rodata.size()));

        for (const auto& [label, local_path] : rodata) {
            std::string dest = dtn_tmp + "/rodata/" + label;
            r = dtn_.run("mkdir -p " + dest);
            tccp_log_ssh("stage:rodata:mkdir", "mkdir -p " + dest, r);

            fs::path src = local_path;
            if (src.is_relative()) {
                src = config_.project_dir() / src;
            }

            if (!fs::exists(src) || !fs::is_directory(src)) {
                tccp_log(fmt::format("WARNING: rodata '{}' missing: {}", label, src.string()));
                if (cb) cb(fmt::format("Warning: rodata '{}' path not found: {}", label, src.string()));
                continue;
            }

            int rodata_count = 0;
            for (const auto& entry : fs::recursive_directory_iterator(src)) {
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), src);
                std::string remote = dest + "/" + rel.string();

                auto parent = rel.parent_path();
                if (!parent.empty() && parent != ".") {
                    dtn_.run("mkdir -p " + dest + "/" + parent.string());
                }

                dtn_.upload(entry.path(), remote);
                rodata_count++;
            }
            tccp_log(fmt::format("uploaded {} rodata files for '{}'", rodata_count, label));
        }
    }

    // ── Create scratch dir on compute node ──
    const char* ssh_opts = "-o StrictHostKeyChecking=no -o BatchMode=yes";
    std::string cmd;

    if (cb) cb("Creating scratch directory on compute node...");
    cmd = fmt::format("ssh {} {} 'mkdir -p {}'", ssh_opts, compute_node, scratch);
    r = dtn_.run(cmd);
    tccp_log_ssh("stage:cn:mkdir", cmd, r);

    // ── Transfer code: DTN temp/code/ contents → compute node scratch root ──
    // Use cd into code/ and tar "." so files land directly in $SCRATCH/
    if (cb) cb("Transferring code to compute node...");
    cmd = fmt::format("cd {}/code && tar cf - . | ssh {} {} 'cd {} && tar xf -'",
                      dtn_tmp, ssh_opts, compute_node, scratch);
    r = dtn_.run(cmd);
    tccp_log_ssh("stage:cn:tar-code", cmd, r);

    // ── Transfer rodata (if any) ──
    if (!rodata.empty()) {
        if (cb) cb("Transferring rodata to compute node...");
        cmd = fmt::format("cd {} && tar cf - rodata | ssh {} {} 'cd {} && tar xf -'",
                          dtn_tmp, ssh_opts, compute_node, scratch);
        r = dtn_.run(cmd);
        tccp_log_ssh("stage:cn:tar-rodata", cmd, r);
    }

    // Verify what landed on the compute node
    cmd = fmt::format("ssh {} {} 'ls -la {}'", ssh_opts, compute_node, scratch);
    r = dtn_.run(cmd);
    tccp_log_ssh("stage:cn:verify", cmd, r);

    // ── Create output symlink on compute node ──
    if (cb) cb("Finalizing compute node...");
    cmd = fmt::format("ssh {} {} 'ln -sfn {} {}/output'",
                      ssh_opts, compute_node, jd + "/output", scratch);
    r = dtn_.run(cmd);
    tccp_log_ssh("stage:cn:symlink", cmd, r);

    // ── Signal ready ──
    cmd = fmt::format("ssh {} {} 'touch {}/.tccp_ready'",
                      ssh_opts, compute_node, scratch);
    r = dtn_.run(cmd);
    tccp_log_ssh("stage:cn:ready", cmd, r);

    // ── Clean up DTN temp ──
    dtn_.run("rm -rf " + dtn_tmp);

    tccp_log("=== stage_to_node complete ===");
    if (cb) cb("Code staged to compute node");
}

void JobManager::collect_remote_logs(const std::string& job_id,
                                     const std::string& slurm_id) {
    std::string jd = job_dir(job_id);

    tccp_log("=== collecting cluster-side logs ===");

    // Fetch cluster-side tccp_debug.log
    auto r = dtn_.run("cat " + jd + "/tccp_debug.log 2>/dev/null");
    if (!r.stdout_data.empty()) {
        tccp_log("--- cluster tccp_debug.log ---");
        tccp_log(r.stdout_data);
        tccp_log("--- end cluster tccp_debug.log ---");
    } else {
        tccp_log("(no cluster tccp_debug.log found)");
    }

    // Fetch slurm stderr
    std::string err_file = jd + "/slurm-" + slurm_id + ".err";
    r = dtn_.run("cat " + err_file + " 2>/dev/null");
    if (!r.stdout_data.empty()) {
        tccp_log("--- slurm stderr ---");
        tccp_log(r.stdout_data);
        tccp_log("--- end slurm stderr ---");
    } else {
        tccp_log("(no slurm stderr found: " + err_file + ")");
    }

    // Fetch slurm stdout
    std::string out_file = jd + "/slurm-" + slurm_id + ".out";
    r = dtn_.run("cat " + out_file + " 2>/dev/null");
    if (!r.stdout_data.empty()) {
        tccp_log("--- slurm stdout ---");
        tccp_log(r.stdout_data);
        tccp_log("--- end slurm stdout ---");
    } else {
        tccp_log("(no slurm stdout found: " + out_file + ")");
    }

    tccp_log("=== end cluster-side logs ===");
}

std::string JobManager::submit(const std::string& job_id, StatusCallback cb) {
    std::string jd = job_dir(job_id);
    std::string script_path = jd + "/run.sh";

    // Write SLURM script via DTN heredoc
    // We already generated it, so we need to pass it through
    // The caller generates and writes; here we just sbatch via login
    std::string cmd = "sbatch " + script_path;
    auto result = login_.run(cmd);

    // Parse SLURM job ID from output like "Submitted batch job 54321"
    std::string output = result.stdout_data;
    std::string slurm_id;
    auto pos = output.find("Submitted batch job ");
    if (pos != std::string::npos) {
        std::string rest = output.substr(pos + 20);
        std::istringstream iss(rest);
        iss >> slurm_id;
    }

    if (slurm_id.empty()) {
        if (cb) cb(fmt::format("sbatch failed: {}", output));
    } else {
        if (cb) cb(fmt::format("Submitted SLURM job {}", slurm_id));
    }

    return slurm_id;
}

JobManager::JobState JobManager::query_job_state(const std::string& slurm_id) {
    std::string cmd = fmt::format("squeue -j {} -o \"%T %N\" -h", slurm_id);
    auto result = login_.run(cmd);

    JobState state;
    if (result.success()) {
        std::istringstream iss(result.stdout_data);
        iss >> state.state >> state.node;
    }
    return state;
}

Result<TrackedJob> JobManager::run(const std::string& job_name, StatusCallback cb) {
    // Start a fresh log section for this run
    tccp_log("========================================");
    tccp_log(fmt::format("=== run job_name={} ===", job_name));

    // Validate job exists in config
    const auto& proj = config_.project();
    if (proj.jobs.find(job_name) == proj.jobs.end()) {
        tccp_log("ERROR: job not found in config");
        return Result<TrackedJob>::Err("No job named '" + job_name + "' in tccp.yaml");
    }

    // Generate job ID
    std::string job_id = generate_job_id(job_name);
    tccp_log(fmt::format("job_id={}", job_id));

    // Ensure directories
    ensure_dirs(job_id, cb);

    // Generate and write SLURM script via DTN
    std::string script_content = generate_slurm_script(job_id, job_name);
    std::string script_path = job_dir(job_id) + "/run.sh";
    tccp_log(fmt::format("slurm script path={}", script_path));
    tccp_log("--- slurm script start ---");
    tccp_log(script_content);
    tccp_log("--- slurm script end ---");

    // Write script via heredoc on DTN
    std::string write_cmd = "cat > " + script_path + " << 'TCCP_SLURM_EOF'\n"
                            + script_content + "\nTCCP_SLURM_EOF";
    auto r = dtn_.run(write_cmd);
    tccp_log_ssh("run:write-script", "cat > " + script_path, r);
    dtn_.run("chmod +x " + script_path);

    // Submit via login
    std::string slurm_id = submit(job_id, cb);
    tccp_log(fmt::format("slurm_id={}", slurm_id));
    if (slurm_id.empty()) {
        tccp_log("ERROR: sbatch failed");
        return Result<TrackedJob>::Err("Failed to submit job");
    }

    // Track the job
    TrackedJob tj;
    tj.job_id = job_id;
    tj.slurm_id = slurm_id;
    tj.job_name = job_name;
    tracked_.push_back(tj);

    return Result<TrackedJob>::Ok(tj);
}

SSHResult JobManager::list(StatusCallback cb) {
    if (cb) cb("Querying job status...");

    std::string cmd = "squeue -u " + username_ +
                      " -o \"%.8i %.20j %.10T %.6M %.4D %R\" -h";
    return login_.run(cmd);
}

SSHResult JobManager::cancel(const std::string& slurm_id, StatusCallback cb) {
    if (cb) cb(fmt::format("Canceling job {}...", slurm_id));
    return login_.run("scancel " + slurm_id);
}

Result<void> JobManager::return_output(const std::string& job_id, StatusCallback cb) {
    std::string remote_output = job_dir(job_id) + "/output";

    // List files in remote output dir
    auto ls_result = dtn_.run("find " + remote_output + " -type f 2>/dev/null");
    if (ls_result.stdout_data.empty()) {
        if (cb) cb("No output files found.");
        return Result<void>::Ok();
    }

    // Parse file list
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

    // Download to local tccp-output/<job-id>/
    fs::path local_base = config_.project_dir() / "tccp-output" / job_id;
    fs::create_directories(local_base);

    int count = 0;
    for (const auto& remote_file : remote_files) {
        // Get relative path from remote_output
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

void JobManager::poll(std::function<void(const TrackedJob&)> on_complete) {
    for (auto& tj : tracked_) {
        if (tj.completed) continue;

        auto state = query_job_state(tj.slurm_id);

        if (state.state.empty() || state.state == "COMPLETED" ||
            state.state == "FAILED" || state.state == "CANCELLED" ||
            state.state == "TIMEOUT") {
            tj.completed = true;
            if (on_complete) on_complete(tj);
        } else if (state.state == "RUNNING" && tj.compute_node.empty()) {
            tj.compute_node = state.node;
        }
    }
}

const std::vector<TrackedJob>& JobManager::tracked_jobs() const {
    return tracked_;
}

TrackedJob* JobManager::find_by_name(const std::string& job_name) {
    TrackedJob* best = nullptr;
    for (auto& tj : tracked_) {
        if (tj.job_name == job_name) {
            best = &tj;
        }
    }
    return best;
}
