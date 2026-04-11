#include "session.hpp"
#include "theme.hpp"
#include <fmt/format.h>
#include <iostream>
#include <chrono>
#include <ctime>
#include <map>
#include <sstream>

// ── Container runtime init ────────────────────────────────
// Finds the best apptainer/singularity binary on compute nodes.
// Prefers suid variants (required when user namespaces are disabled).
static std::string container_runtime_init() {
    return
        "type module &>/dev/null || . /etc/profile 2>/dev/null || true; "
        "SUID_MOD=$(module -t avail apptainer 2>&1 | grep suid | grep -v no-suid | sort -V | tail -1); "
        "module load ${SUID_MOD:-apptainer} 2>/dev/null || module load singularity 2>/dev/null || true; "
        "CEXE=$(command -v apptainer 2>/dev/null || command -v singularity 2>/dev/null || echo singularity)";
}

// ── Construction ──────────────────────────────────────────

Session::Session(const Config& cfg, SSH& ssh, Sync& sync, StateStore& store)
    : cfg_(cfg), ssh_(ssh), sync_(sync), store_(store) {
    if (store_.exists()) {
        state_ = store_.load();
    }
}

// ── Path helpers ──────────────────────────────────────────

std::string Session::scratch_path() const {
    return fmt::format("/tmp/{}/{}", cfg_.global.user, cfg_.project_name);
}

std::string Session::sif_path() const {
    if (cfg_.global.cache_containers) {
        return fmt::format("~/.tccp/containers/{}", sif_name(cfg_.project.container));
    }
    return fmt::format("/tmp/{}/containers/{}", cfg_.global.user, sif_name(cfg_.project.container));
}

std::string Session::socket_path() const {
    return scratch_path() + "/.tccp.sock";
}

std::string Session::nfs_output() const {
    return fmt::format("~/.tccp/projects/{}/output", cfg_.project_name);
}

std::string Session::tccp_home() const {
    return "~/.tccp";
}

std::string Session::dtach_bin() const {
    return tccp_home() + "/bin/dtach";
}

// ── Active check ──────────────────────────────────────────

bool Session::active() const {
    return !state_.slurm_id.empty();
}

// ── Start ─────────────────────────────────────────────────

Result<void> Session::start(StatusCallback cb) {
    if (active()) {
        return Result<void>::Err(fmt::format(
            "Session already running (job {}). Use 'tccp stop' first.", state_.slurm_id));
    }

    // 1. Allocate
    auto alloc_result = allocate(cb);
    if (alloc_result.is_err()) return Result<void>::Err(alloc_result.error);
    std::string job_id = alloc_result.value;

    // 2. Wait for node
    auto node_result = wait_for_node(job_id, cb);
    if (node_result.is_err()) {
        ssh_.run_login("scancel " + job_id);
        return Result<void>::Err(node_result.error);
    }
    std::string node = node_result.value;

    // Save state early
    state_.slurm_id = job_id;
    state_.compute_node = node;
    state_.scratch = scratch_path();
    state_.container_uri = docker_uri(cfg_.project.container);
    state_.container_sif = sif_path();
    store_.save(state_);

    // 3. Ensure container
    auto container_result = ensure_container(node, cb);
    if (container_result.is_err()) {
        ssh_.run_login("scancel " + job_id);
        store_.clear();
        return container_result;
    }

    // 4. Verify container runtime works
    {
        if (cb) cb("Verifying container runtime...");
        auto test_cmd = singularity_cmd(scratch_path(), "echo CONTAINER_EXEC_OK");
        auto test = ssh_.run_compute(node, test_cmd, 30);
        if (test.out.find("CONTAINER_EXEC_OK") == std::string::npos) {
            // Gather diagnostics via direct SSH (no singularity)
            auto diag = ssh_.run_compute(node,
                "type module &>/dev/null || . /etc/profile 2>/dev/null || true; "
                "module load apptainer 2>/dev/null || module load singularity 2>/dev/null || true; "
                "echo \"binary: $(command -v apptainer 2>/dev/null || command -v singularity 2>/dev/null || echo NONE)\"; "
                "echo \"version: $(apptainer --version 2>/dev/null || singularity --version 2>/dev/null || echo NONE)\"; "
                "echo \"userns: $(cat /proc/sys/user/max_user_namespaces 2>/dev/null || echo unavailable)\"; "
                "echo \"suid: $(ls $(dirname $(command -v apptainer 2>/dev/null || command -v singularity 2>/dev/null) 2>/dev/null)/../libexec/*/bin/starter-suid 2>/dev/null || echo NONE)\"; "
                "echo \"modules: $(module avail -t apptainer singularity 2>&1 | tr '\\n' ' ')\"");
            ssh_.run_login("scancel " + job_id);
            store_.clear();
            return Result<void>::Err(fmt::format(
                "Container runtime cannot create namespaces\n{}", diag.out));
        }
    }

    // 5. Ensure dtach
    auto dtach_result = ensure_dtach(cb);
    if (dtach_result.is_err()) {
        ssh_.run_login("scancel " + job_id);
        store_.clear();
        return dtach_result;
    }

    // 6. Sync project files
    if (cb) cb("Syncing project files...");
    auto sync_result = sync_.push(node, scratch_path(), state_, cb);
    if (sync_result.is_err()) {
        ssh_.run_login("scancel " + job_id);
        store_.clear();
        return sync_result;
    }

    // 7. Create output dirs and env script
    ssh_.run(fmt::format("mkdir -p {}", nfs_output()));
    ssh_.run_compute(node, fmt::format("mkdir -p {}/output", scratch_path()));
    auto env_script = build_env_script();
    ssh_.run_compute(node, fmt::format(
        "cat > {}/.tccp-env.sh << 'TCCP_ENV_EOF'\n{}\nTCCP_ENV_EOF",
        scratch_path(), env_script));

    // 8. Run init
    auto init_result = run_init(node, scratch_path(), cb);
    if (init_result.is_err()) {
        ssh_.run_login("scancel " + job_id);
        store_.clear();
        return init_result;
    }

    // 9. Start dtach
    auto dtach_start = start_dtach(node, scratch_path(), cb);
    if (dtach_start.is_err()) {
        ssh_.run_login("scancel " + job_id);
        store_.clear();
        return dtach_start;
    }

    // 10. Save final state
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    state_.started_at = std::ctime(&time_t);
    state_.started_at = trim(state_.started_at);
    store_.save(state_);

    // Print status
    if (cb) cb(fmt::format("Session started on {}", node));
    if (!cfg_.project.ports.empty()) {
        std::string port_list;
        for (int p : cfg_.project.ports) {
            if (!port_list.empty()) port_list += ", ";
            port_list += std::to_string(p);
        }
        if (cb) cb(fmt::format("Ports: {} (forwarded during 'tccp shell')", port_list));
    }

    return Result<void>::Ok();
}

// ── GPU selection ─────────────────────────────────────────

std::string Session::pick_gpu(const std::string& partition, StatusCallback cb) {
    // VRAM ranking (higher = better)
    struct GpuInfo { int vram; int avail; };
    std::map<std::string, GpuInfo> gpus;

    // Static VRAM values
    std::map<std::string, int> vram_gb = {
        {"t4", 16}, {"p100", 16}, {"rtx_6000", 24}, {"v100", 32},
        {"rtx_a5000", 24}, {"l40", 48}, {"rtx_a6000", 48},
        {"rtx_6000ada", 48}, {"l40s", 48}, {"a100", 80}, {"h100", 80},
    };

    auto result = ssh_.run_login("sinfo -h -o '%P|%G|%D|%T' 2>/dev/null");
    if (!result.ok()) return "";

    std::istringstream iss(result.out);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;

        std::istringstream lss(line);
        std::string f;
        std::vector<std::string> fields;
        while (std::getline(lss, f, '|')) fields.push_back(trim(f));
        if (fields.size() < 4) continue;

        std::string part = fields[0];
        if (!part.empty() && part.back() == '*') part.pop_back();
        if (part != partition) continue;

        std::string gres = fields[1];
        std::string gres_lower = gres;
        std::transform(gres_lower.begin(), gres_lower.end(), gres_lower.begin(), ::tolower);
        auto gpos = gres_lower.find("gpu:");
        if (gpos == std::string::npos) continue;

        std::string rest = gres.substr(gpos + 4);
        std::string gpu_type;
        int gpu_count = 0;
        auto colon = rest.find(':');
        if (colon != std::string::npos) {
            gpu_type = rest.substr(0, colon);
            try { gpu_count = std::stoi(rest.substr(colon + 1)); } catch (...) {}
        } else {
            gpu_type = rest;
            gpu_count = 1;
        }
        if (gpu_count <= 0) continue;

        int nodes = 0;
        try { nodes = std::stoi(fields[2]); } catch (...) {}

        std::string state = fields[3];
        std::transform(state.begin(), state.end(), state.begin(), ::tolower);
        bool avail = state.find("idle") != std::string::npos ||
                     state.find("mix") != std::string::npos;

        if (gpus.find(gpu_type) == gpus.end()) {
            int v = 0;
            auto it = vram_gb.find(gpu_type);
            if (it != vram_gb.end()) v = it->second;
            gpus[gpu_type] = {v, 0};
        }
        if (avail) gpus[gpu_type].avail += nodes * gpu_count;
    }

    // Pick: best available GPU by VRAM (among those with availability)
    std::string best;
    int best_vram = 0;
    for (const auto& [name, info] : gpus) {
        if (info.avail > 0 && info.vram > best_vram) {
            best = name;
            best_vram = info.vram;
        }
    }

    // Fallback: if nothing has availability, pick highest VRAM anyway (will queue)
    if (best.empty()) {
        for (const auto& [name, info] : gpus) {
            if (info.vram > best_vram) {
                best = name;
                best_vram = info.vram;
            }
        }
    }

    return best;
}

// ── Allocate ──────────────────────────────────────────────

Result<std::string> Session::allocate(StatusCallback cb) {
    std::string partition = cfg_.global.partition;
    std::string time_str = parse_time(cfg_.project.time);
    std::string gpu = cfg_.project.gpu;
    int gpu_count = cfg_.project.gpu_count;

    // Auto-pick GPU if not specified
    if (gpu.empty() || gpu == "auto") {
        if (cb) cb("Checking GPU availability...");
        gpu = pick_gpu(partition, cb);
        if (gpu.empty()) {
            return Result<std::string>::Err(
                fmt::format("No GPUs found on partition '{}'", partition));
        }
        if (cb) cb(fmt::format("Selected {} GPU", gpu));
    }

    if (cb) cb(fmt::format("Requesting {} on {}...", gpu, partition));

    std::string cmd = fmt::format(
        "sbatch --parsable --wrap='sleep infinity' -p {} -c {} --mem={} -t {} -J tccp-{}",
        partition, cfg_.project.cpus, cfg_.project.memory, time_str, cfg_.project_name);

    cmd += fmt::format(" --gres=gpu:{}:{}", gpu, gpu_count);

    auto result = ssh_.run_login(cmd);
    if (!result.ok()) {
        return Result<std::string>::Err(fmt::format("sbatch failed: {}", result.err));
    }

    std::string job_id = trim(result.out);
    if (job_id.empty()) {
        return Result<std::string>::Err("sbatch returned empty job ID");
    }

    if (cb) cb(fmt::format("Allocation {} submitted", job_id));
    return Result<std::string>::Ok(job_id);
}

// ── Wait for node ─────────────────────────────────────────

Result<std::string> Session::wait_for_node(const std::string& id, StatusCallback cb) {
    if (cb) cb("Waiting for allocation...");

    for (int i = 0; i < 600; i++) {  // 30 min max
        auto result = ssh_.run_login(fmt::format("squeue -j {} -o '%T %N' -h", id));
        if (!result.ok() || result.out.empty()) {
            return Result<std::string>::Err("Job disappeared from queue");
        }

        std::string out = trim(result.out);
        if (out.find("RUNNING") == 0) {
            std::string node = out.substr(8);  // "RUNNING " is 8 chars
            node = trim(node);
            if (!node.empty()) {
                if (cb) cb(fmt::format("Running on {}", node));
                return Result<std::string>::Ok(node);
            }
        }

        if (out.find("FAILED") != std::string::npos ||
            out.find("CANCELLED") != std::string::npos) {
            return Result<std::string>::Err(fmt::format("Job {}: {}", id, out));
        }

        sleep_ms(3000);
    }

    return Result<std::string>::Err("Timed out waiting for allocation");
}

// ── Container ─────────────────────────────────────────────

Result<void> Session::ensure_container(const std::string& node, StatusCallback cb) {
    std::string sif = sif_path();
    bool cached = cfg_.global.cache_containers;

    if (cb) cb("Checking for container image...");

    // Check in the right place: DTN for NFS cache, compute node for /tmp
    auto check = cached
        ? ssh_.run("test -f " + sif + " && echo IMG_OK || echo IMG_MISSING")
        : ssh_.run_compute(node, "test -f " + sif + " && echo IMG_OK || echo IMG_MISSING");

    if (check.out.find("IMG_OK") != std::string::npos) {
        if (cb) cb(cached ? "Container image cached (NFS)" : "Container image cached on node");
        return Result<void>::Ok();
    }

    if (cb) cb(fmt::format("Downloading {}...", cfg_.project.container));

    // Pull always on compute node (large /tmp for temp files)
    std::string cache_dir = fmt::format("/tmp/{}/singularity-cache", cfg_.global.user);
    std::string tmp_dir = fmt::format("/tmp/{}/singularity-tmp", cfg_.global.user);
    std::string uri = docker_uri(cfg_.project.container);

    // Ensure target directory exists
    if (cached) {
        ssh_.run("mkdir -p ~/.tccp/containers");
    } else {
        std::string container_dir = fmt::format("/tmp/{}/containers", cfg_.global.user);
        ssh_.run_compute(node, "mkdir -p " + container_dir);
    }

    // Layer cache survives across pulls — re-pull only rebuilds SIF, skips download
    // Use APPTAINER_ env vars (preferred) with SINGULARITY_ fallbacks
    // Add common mksquashfs locations to PATH (needed for SIF conversion)
    auto result = ssh_.run_compute(node, fmt::format(
        "{}; mkdir -p {} {}; "
        "export PATH=$PATH:/usr/sbin:/sbin; "
        "APPTAINER_CACHEDIR={} APPTAINER_TMPDIR={} "
        "SINGULARITY_CACHEDIR={} SINGULARITY_TMPDIR={} "
        "$CEXE pull --force {} {} && "
        "rm -rf {}",
        container_runtime_init(), cache_dir, tmp_dir,
        cache_dir, tmp_dir,
        cache_dir, tmp_dir,
        sif, uri,
        tmp_dir), 1800);

    if (!result.ok()) {
        std::string detail = result.err.empty() ? result.out : result.err;
        return Result<void>::Err(fmt::format("Container pull failed: {}", detail));
    }

    // Verify from correct location
    auto verify = cached
        ? ssh_.run("test -f " + sif + " && echo IMG_OK || echo IMG_FAIL")
        : ssh_.run_compute(node, "test -f " + sif + " && echo IMG_OK || echo IMG_FAIL");

    if (verify.out.find("IMG_FAIL") != std::string::npos) {
        return Result<void>::Err("Container pull failed: image not found after pull");
    }

    auto sz = cached
        ? ssh_.run(fmt::format("du -sh {} 2>/dev/null | cut -f1", sif))
        : ssh_.run_compute(node, fmt::format("du -sh {} 2>/dev/null | cut -f1", sif));

    if (cb) cb(fmt::format("Container ready ({})", trim(sz.out).empty() ? "?" : trim(sz.out)));
    return Result<void>::Ok();
}

// ── dtach ─────────────────────────────────────────────────

Result<void> Session::ensure_dtach(StatusCallback cb) {
    std::string bin = dtach_bin();
    auto check = ssh_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_MISSING");
    if (check.out.find("DTACH_OK") != std::string::npos) {
        return Result<void>::Ok();
    }

    // Check system dtach
    auto sys = ssh_.run("which dtach 2>/dev/null");
    if (sys.ok() && !sys.out.empty()) {
        ssh_.run("mkdir -p ~/.tccp/bin");
        ssh_.run(fmt::format("cp {} {} && chmod +x {}", trim(sys.out), bin, bin));
        auto v = ssh_.run("test -x " + bin + " && echo OK");
        if (v.out.find("OK") != std::string::npos) {
            if (cb) cb("dtach: copied from system");
            return Result<void>::Ok();
        }
    }

    if (cb) cb("Building dtach...");
    ssh_.run("mkdir -p ~/.tccp/bin");

    std::string build_dir = "~/.tccp/dtach-build";
    ssh_.run("rm -rf " + build_dir);

    auto clone = ssh_.run("git clone https://github.com/crigler/dtach.git " + build_dir + " 2>&1", 60);
    if (!clone.ok()) {
        if (cb) cb("git not available, trying curl...");
        ssh_.run("mkdir -p " + build_dir);
        auto dl = ssh_.run("curl -sL https://github.com/crigler/dtach/archive/refs/heads/master.tar.gz "
                           "| tar xz -C " + build_dir + " --strip-components=1 2>&1", 60);
        if (!dl.ok()) {
            return Result<void>::Err(fmt::format("Cannot download dtach: {}", dl.out));
        }
    }

    if (cb) cb("Compiling dtach...");
    auto build = ssh_.run("cd " + build_dir + " && cc -o dtach dtach.c master.c attach.c -lutil 2>&1", 60);
    if (!build.ok()) {
        if (cb) cb("Direct compile failed, trying configure/make...");
        auto build2 = ssh_.run("cd " + build_dir + " && ./configure && make 2>&1", 120);
        if (!build2.ok()) {
            return Result<void>::Err(fmt::format("Failed to compile dtach: {}", build2.out));
        }
    }

    ssh_.run(fmt::format("cp {}/dtach {} && chmod +x {}", build_dir, bin, bin));
    ssh_.run("rm -rf " + build_dir);

    auto verify = ssh_.run("test -x " + bin + " && echo DTACH_OK || echo DTACH_FAIL");
    if (verify.out.find("DTACH_FAIL") != std::string::npos) {
        return Result<void>::Err("Failed to install dtach");
    }

    if (cb) cb("dtach ready");
    return Result<void>::Ok();
}

// ── Singularity command builder ───────────────────────────

std::string Session::singularity_cmd(const std::string& scratch, const std::string& inner) const {
    std::string nv = "--nv ";
    std::string binds = fmt::format("-B {}:{}", nfs_output(), scratch + "/output");

    // Bind rodata directories
    for (const auto& rd : cfg_.project.rodata) {
        std::string rd_clean = rd;
        while (!rd_clean.empty() && rd_clean.back() == '/') rd_clean.pop_back();
        binds += fmt::format(" -B {}/{rd}:{}/{rd}",
                             fmt::format("/cluster/home/{}", cfg_.global.user), scratch,
                             fmt::arg("rd", rd_clean));
    }

    return fmt::format(
        "{}; cd {}; $CEXE exec --env \"PS1=tccp> \" --env TERM=xterm-256color {}{} {} {}",
        container_runtime_init(), scratch, nv, binds, sif_path(), inner);
}

// ── Env script ────────────────────────────────────────────

std::string Session::build_env_script() const {
    std::string scratch = scratch_path();
    return fmt::format(
        "export PYTHONUSERBASE={scratch}/.local\n"
        "export PATH=$PYTHONUSERBASE/bin:$PATH\n"
        "export TCCP_PROJECT={project}\n"
        "export TCCP_SCRATCH={scratch}\n"
        "export TERM=${{TERM:-xterm-256color}}\n"
        "export PS1=\"tccp> \"\n"
        "cd {scratch}\n",
        fmt::arg("scratch", scratch),
        fmt::arg("project", cfg_.project_name));
}

// ── Init ──────────────────────────────────────────────────

Result<void> Session::run_init(const std::string& node, const std::string& scratch,
                               StatusCallback cb) {
    std::string init_cmd = cfg_.project.init;

    // Fallback to tccp_init.sh if it exists
    if (init_cmd.empty()) {
        auto check = ssh_.run_compute(node, fmt::format("test -f {}/tccp_init.sh && echo YES", scratch));
        if (check.out.find("YES") != std::string::npos) {
            init_cmd = "bash tccp_init.sh";
        }
    }

    if (init_cmd.empty()) {
        if (cb) cb("No init command, skipping");
        return Result<void>::Ok();
    }

    if (cb) cb(fmt::format("Running init: {}", init_cmd));
    auto cmd = singularity_cmd(scratch, fmt::format("bash -c 'source .tccp-env.sh && {}'", init_cmd));
    auto result = ssh_.run_compute(node, cmd, 600);
    if (!result.ok()) {
        return Result<void>::Err(fmt::format("Init failed (exit {}): {}", result.exit_code, result.out));
    }
    if (cb) cb("Init complete");
    return Result<void>::Ok();
}

// ── Start dtach ───────────────────────────────────────────

Result<void> Session::start_dtach(const std::string& node, const std::string& scratch,
                                  StatusCallback cb) {
    if (cb) cb("Starting shell session...");

    std::string sock = socket_path();

    // Remove stale socket from previous failed/interrupted sessions
    ssh_.run_compute(node, "rm -f " + sock);

    std::string inner = fmt::format("bash --rcfile {scratch}/.tccp-env.sh",
                                    fmt::arg("scratch", scratch));
    std::string cmd = singularity_cmd(scratch, inner);

    auto result = ssh_.run_compute(node, fmt::format("{} -n {} bash -c {}",
                                                     dtach_bin(), sock, escape_for_ssh(cmd)));
    if (!result.ok()) {
        std::string detail = result.err.empty() ? result.out : result.err;
        return Result<void>::Err(fmt::format("Failed to start dtach (exit {}): {}", result.exit_code, detail));
    }

    // Verify socket exists
    sleep_ms(500);
    auto check = ssh_.run_compute(node, "test -S " + sock + " && echo OK");
    if (check.out.find("OK") == std::string::npos) {
        return Result<void>::Err("dtach socket not found after launch");
    }

    if (cb) cb("Shell session ready");
    return Result<void>::Ok();
}

// ── Shell (attach loop) ──────────────────────────────────

int Session::shell() {
    if (!active()) {
        std::cerr << theme::error("No active session. Run 'tccp start' first.");
        return 1;
    }

    std::string node = state_.compute_node;
    std::string sock = socket_path();

    // Verify job is still running
    auto job_check = ssh_.run_login(fmt::format("squeue -j {} -h -o '%T'", state_.slurm_id));
    std::string job_state = trim(job_check.out);
    if (!job_check.ok() || job_state.empty() ||
        (job_state.find("RUNNING") == std::string::npos && job_state.find("PENDING") == std::string::npos)) {
        std::cerr << theme::error(fmt::format("Job {} is no longer running.", state_.slurm_id));
        store_.clear();
        state_ = SessionState{};
        std::cerr << theme::error("Session cleared. Run 'tccp start' to begin a new session.");
        return 1;
    }

    // Verify socket exists
    auto sock_check = ssh_.run_compute(node, "test -S " + sock + " && echo OK");
    if (sock_check.out.find("OK") == std::string::npos) {
        std::cerr << theme::error("Shell session not found (dtach socket missing).");
        std::cerr << theme::error("Run 'tccp stop' then 'tccp start' to create a new session.");
        return 1;
    }

    std::cout << theme::step("Ctrl+S to sync | Ctrl+D to exit") << std::flush;

    while (true) {
        // Attach to dtach with Ctrl+S as detach key
        std::string attach_cmd = fmt::format("{} -a {} -e ^S", dtach_bin(), sock);
        std::string inner_cmd = singularity_cmd(state_.scratch,
            fmt::format("bash -c '{}'", attach_cmd));

        int rc = ssh_.interactive(node, inner_cmd, cfg_.project.ports);

        // Check if dtach socket still exists
        auto check = ssh_.run_compute(node, "test -S " + sock + " && echo ALIVE");
        if (check.out.find("ALIVE") == std::string::npos) {
            // Shell exited (Ctrl+D / exit)
            auto job = ssh_.run_login(fmt::format("squeue -j {} -h -o '%T'", state_.slurm_id));
            if (!job.ok() || trim(job.out).empty()) {
                store_.clear();
                state_ = SessionState{};
                std::cout << theme::ok("Session ended (job completed). Run 'tccp start' for a new session.");
            } else {
                std::cout << theme::ok("Shell exited. Run 'tccp shell' to reconnect or 'tccp stop' to end.");
            }
            return 0;
        }

        // Ctrl+] was pressed — sync and reattach
        std::cout << theme::step("Syncing...");
        sync_.refresh(node, state_.scratch, state_, [](const std::string& msg) {
            std::cout << theme::step(msg);
        });
        store_.save(state_);
        std::cout << theme::step("Reattaching...");
    }
}

// ── Exec ──────────────────────────────────────────────────

Result<int> Session::exec(const std::string& cmd) {
    if (!active()) {
        return Result<int>::Err("No active session. Run 'tccp start' first.");
    }

    std::string full = singularity_cmd(state_.scratch,
        fmt::format("bash -c 'source {scratch}/.tccp-env.sh && {cmd}'",
                    fmt::arg("scratch", state_.scratch),
                    fmt::arg("cmd", cmd)));

    auto result = ssh_.run_compute(state_.compute_node, full, 0);
    if (!result.out.empty()) std::cout << result.out << "\n";
    if (!result.err.empty()) std::cerr << result.err << "\n";

    return Result<int>::Ok(result.exit_code);
}

// ── Sync ──────────────────────────────────────────────────

Result<void> Session::sync_files(StatusCallback cb) {
    if (!active()) {
        return Result<void>::Err("No active session. Run 'tccp start' first.");
    }
    auto result = sync_.refresh(state_.compute_node, state_.scratch, state_, cb);
    if (result.is_ok()) store_.save(state_);
    return result;
}

// ── Status ────────────────────────────────────────────────

void Session::status() {
    if (!active()) {
        std::cout << theme::ok("No active session.");
        return;
    }

    std::cout << theme::section("Session");
    std::cout << theme::kv("Job ID", state_.slurm_id);
    std::cout << theme::kv("Node", state_.compute_node);
    std::cout << theme::kv("Scratch", state_.scratch);
    std::cout << theme::kv("Container", cfg_.project.container);
    std::cout << theme::kv("Started", state_.started_at);

    if (!cfg_.project.ports.empty()) {
        std::string port_list;
        for (int p : cfg_.project.ports) {
            if (!port_list.empty()) port_list += ", ";
            port_list += std::to_string(p);
        }
        std::cout << theme::kv("Ports", port_list + " (forwarded during 'tccp shell')");
    }

    // Check SLURM job status
    auto result = ssh_.run_login(fmt::format("squeue -j {} -o '%T %M' -h", state_.slurm_id));
    if (result.ok() && !result.out.empty()) {
        std::string out = trim(result.out);
        std::cout << theme::kv("Status", out);
    } else {
        std::cout << theme::kv("Status", "unknown (job may have ended)");
    }
}

// ── Stop ──────────────────────────────────────────────────

Result<void> Session::stop(StatusCallback cb) {
    if (!active()) {
        return Result<void>::Err("No active session.");
    }

    // Check if job is still running
    auto job_check = ssh_.run_login(fmt::format("squeue -j {} -h -o '%T'", state_.slurm_id));
    bool job_alive = job_check.ok() && !trim(job_check.out).empty();

    if (job_alive) {
        // Pull output before canceling
        if (cb) cb("Pulling output...");
        sync_.pull_output(cb);

        if (cb) cb(fmt::format("Canceling job {}...", state_.slurm_id));
        ssh_.run_login("scancel " + state_.slurm_id);
    } else {
        if (cb) cb("Job already ended.");
    }

    // Always clear state
    store_.clear();
    state_ = SessionState{};

    if (cb) cb("Session stopped.");
    return Result<void>::Ok();
}
