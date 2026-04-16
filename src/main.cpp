#include "config.hpp"
#include "ssh.hpp"
#include "sync.hpp"
#include "session.hpp"
#include "state.hpp"
#include "theme.hpp"

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

static StatusCallback make_cb() {
    return [](const std::string& msg) {
        std::cout << theme::step(msg);
    };
}

static int run_with_session(const std::function<int(Session&)>& fn) {
    auto cfg_result = load_config();
    if (cfg_result.is_err()) {
        std::cerr << theme::error(cfg_result.error);
        return 1;
    }
    auto& cfg = cfg_result.value;

    SSH ssh(cfg.global.host, cfg.global.login, cfg.global.user, cfg.global.password);
    auto conn = ssh.connect();
    if (conn.is_err()) {
        std::cerr << theme::error(conn.error);
        return 1;
    }

    Sync sync(ssh, cfg);
    StateStore store(cfg.project_name);
    Session session(cfg, ssh, sync, store);

    return fn(session);
}

// ── GPU info (static, no SSH) ─────────────────────────────

static void show_gpu_guide() {
    std::cout << "\n"
              << theme::color::BOLD << "  GPU Guide"
              << theme::color::RESET << theme::color::DIM
              << " (least to most powerful)" << theme::color::RESET << "\n\n";

    struct G { const char* name; const char* vram; const char* use; const char* notes; };
    G gpus[] = {
        {"t4",          "16 GB", "Inference, light training",
         "Oldest Turing card. Slow FP32 but decent FP16. Good for small models and debugging."},
        {"p100",        "16 GB", "Legacy workloads",
         "Pascal generation. No tensor cores. Mainly useful when nothing else is free."},
        {"rtx_6000",    "24 GB", "Medium training",
         "Turing with tensor cores. Decent for mid-size models."},
        {"v100",        "32 GB", "General training",
         "Volta. First-gen tensor cores. Solid all-rounder, good FP16 throughput."},
        {"a100-40gb",   "40 GB", "Large model training",
         "Ampere. 3rd-gen tensor cores, TF32, BF16. Fast interconnect. Great price/perf."},
        {"rtx_a5000",   "24 GB", "Medium training, multi-GPU",
         "Ampere workstation GPU. 8 per node — good for data-parallel training."},
        {"l40",         "48 GB", "Large models, fine-tuning",
         "Ada Lovelace. Good VRAM for big batch sizes or large models."},
        {"rtx_a6000",   "48 GB", "Large models",
         "Ampere workstation. 48 GB VRAM, 8 per node."},
        {"rtx_6000ada", "48 GB", "Large models, fast training",
         "Ada Lovelace workstation. Newer arch than rtx_a6000."},
        {"a100-80gb",   "80 GB", "LLMs, very large models",
         "The workhorse. 80 GB VRAM handles most LLM fine-tuning. High bandwidth."},
        {"l40s",        "48 GB", "Fast training, inference",
         "Ada Lovelace server GPU. Better FP8 than L40. Newer generation."},
        {"h100",        "80 GB", "Largest models, fastest training",
         "Hopper. 4th-gen tensor cores, FP8, transformer engine. Top of the line."},
    };

    for (const auto& g : gpus) {
        std::cout << "  " << theme::color::BOLD << fmt::format("{:<14}", g.name)
                  << theme::color::RESET
                  << theme::color::DIM << fmt::format("{:>6} VRAM", g.vram) << theme::color::RESET
                  << "  " << g.use << "\n"
                  << theme::color::DIM << fmt::format("  {:<14}{}", "", g.notes)
                  << theme::color::RESET << "\n";
    }

    std::cout << "\n" << theme::color::DIM
              << "  Partitions: 'gpu' is dedicated. 'preempt' is shared (may be interrupted).\n"
              << "  Use 'tccp gpus' to see live availability.\n"
              << "  Config: gpu: a100-80gb  gpu-count: 1  partition: preempt\n"
              << theme::color::RESET << "\n";
}

// ── GPU live availability (SSH to login node) ─────────────

static int show_gpu_availability(const std::string& filter = "") {
    auto cfg_result = load_config_global_only();
    if (cfg_result.is_err()) {
        std::cerr << theme::error(cfg_result.error);
        return 1;
    }
    auto& g = cfg_result.value;

    SSH ssh(g.host, g.login, g.user, g.password);
    auto conn = ssh.connect();
    if (conn.is_err()) {
        std::cerr << theme::error(conn.error);
        return 1;
    }

    auto result = ssh.run_login(
        "sinfo -h -o '%P|%G|%D|%m|%c|%T' 2>/dev/null");
    if (!result.ok() || result.out.empty()) {
        std::cerr << theme::error("Could not query GPU resources");
        return 1;
    }

    // Lowercase filter for matching
    std::string filter_lower = filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);

    // VRAM lookup
    std::map<std::string, std::string> vram = {
        {"t4", "16 GB"}, {"p100", "16 GB"}, {"rtx_6000", "24 GB"},
        {"v100", "32 GB"}, {"a100", "80 GB"}, {"rtx_a5000", "24 GB"},
        {"l40", "48 GB"}, {"rtx_a6000", "48 GB"}, {"rtx_6000ada", "48 GB"},
        {"l40s", "48 GB"}, {"h100", "80 GB"},
    };

    // Detailed row for filtered view
    struct DetailRow {
        std::string partition, gpu, state;
        int gpu_per_node = 0, nodes = 0, gpus_avail = 0, gpus_total = 0;
        int mem_gb = 0, cpus = 0;
    };
    std::vector<DetailRow> detail_rows;

    // Aggregated row for summary view
    struct GpuAgg {
        std::string gpu;
        int avail = 0, total = 0;
        std::set<std::string> partitions;
    };
    std::map<std::string, GpuAgg> agg;
    std::vector<std::string> order;

    std::istringstream iss(result.out);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;

        std::vector<std::string> fields;
        std::istringstream lss(line);
        std::string field;
        while (std::getline(lss, field, '|')) fields.push_back(trim(field));
        if (fields.size() < 6) continue;

        // Check for GPU in GRES
        std::string gres = fields[1];
        std::string gres_lower = gres;
        std::transform(gres_lower.begin(), gres_lower.end(), gres_lower.begin(), ::tolower);
        if (gres_lower.find("gpu") == std::string::npos) continue;

        // Parse gpu:type:count
        std::string gpu_type;
        int gpu_count = 0;
        auto gpos = gres_lower.find("gpu:");
        if (gpos == std::string::npos) continue;
        std::string rest = gres.substr(gpos + 4);
        auto colon = rest.find(':');
        if (colon != std::string::npos) {
            gpu_type = rest.substr(0, colon);
            try { gpu_count = std::stoi(rest.substr(colon + 1)); } catch (...) {}
        } else {
            gpu_type = rest;
            gpu_count = 1;
        }
        if (gpu_count <= 0) continue;

        std::string partition = fields[0];
        if (!partition.empty() && partition.back() == '*') partition.pop_back();

        int nodes = 0;
        try { nodes = std::stoi(fields[2]); } catch (...) {}

        std::string state = fields[5];
        std::string state_lower = state;
        std::transform(state_lower.begin(), state_lower.end(), state_lower.begin(), ::tolower);
        bool is_avail = state_lower.find("idle") != std::string::npos ||
                        state_lower.find("mix") != std::string::npos;

        int mem_mb = 0;
        try { mem_mb = std::stoi(fields[3]); } catch (...) {}
        int cpus_val = 0;
        try { cpus_val = std::stoi(fields[4]); } catch (...) {}

        // Aggregate for summary
        if (agg.find(gpu_type) == agg.end()) {
            agg[gpu_type] = {gpu_type, 0, 0, {}};
            order.push_back(gpu_type);
        }
        auto& a = agg[gpu_type];
        a.total += nodes * gpu_count;
        if (is_avail) a.avail += nodes * gpu_count;
        a.partitions.insert(partition);

        // Collect detail rows for filtered view
        if (!filter_lower.empty() && gpu_type == filter_lower) {
            DetailRow dr;
            dr.partition = partition;
            dr.gpu = gpu_type;
            dr.state = state;
            dr.gpu_per_node = gpu_count;
            dr.nodes = nodes;
            dr.gpus_avail = is_avail ? nodes * gpu_count : 0;
            dr.gpus_total = nodes * gpu_count;
            dr.mem_gb = mem_mb / 1024;
            dr.cpus = cpus_val;
            detail_rows.push_back(dr);
        }
    }

    // ── Filtered detail view ─────────────────────────────────
    if (!filter_lower.empty()) {
        if (detail_rows.empty()) {
            std::cerr << theme::error(fmt::format("No GPU '{}' found on this cluster", filter));
            return 1;
        }

        std::string v = "?";
        auto vit = vram.find(filter_lower);
        if (vit != vram.end()) v = vit->second;

        std::cout << "\n"
                  << theme::color::BOLD << "  " << filter_lower << theme::color::RESET
                  << theme::color::DIM << "  " << v << " VRAM" << theme::color::RESET << "\n\n";

        std::string hfmt = "  {:<12}{:<10}{:<8}{:<8}{:<8}{:<8}{}\n";
        std::cout << theme::color::DIM
                  << fmt::format(fmt::runtime(hfmt), "PARTITION", "GPU/NODE", "AVAIL", "TOTAL", "MEM", "CPUS", "STATE")
                  << theme::color::RESET;

        for (const auto& dr : detail_rows) {
            std::cout << fmt::format(fmt::runtime(hfmt),
                dr.partition,
                std::to_string(dr.gpu_per_node),
                std::to_string(dr.gpus_avail),
                std::to_string(dr.gpus_total),
                fmt::format("{}G", dr.mem_gb),
                std::to_string(dr.cpus),
                dr.state);
        }

        // Totals
        int total_avail = 0, total_total = 0;
        for (const auto& dr : detail_rows) {
            total_avail += dr.gpus_avail;
            total_total += dr.gpus_total;
        }
        std::cout << "\n" << theme::color::DIM
                  << fmt::format("  {} of {} {} GPUs available\n", total_avail, total_total, filter_lower)
                  << theme::color::RESET << "\n";
        return 0;
    }

    // ── Summary view ─────────────────────────────────────────
    if (agg.empty()) {
        std::cout << "No GPU resources found.\n";
        return 0;
    }

    // Sort by availability (most free first)
    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        return agg[a].avail > agg[b].avail;
    });

    std::cout << "\n";
    std::string hfmt = "  {:<16}{:<10}{:<8}{:<8}{}\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(hfmt), "GPU", "VRAM", "AVAIL", "TOTAL", "PARTITIONS")
              << theme::color::RESET;

    for (const auto& key : order) {
        auto& a = agg[key];
        std::string v = "?";
        auto it = vram.find(a.gpu);
        if (it != vram.end()) v = it->second;

        std::string parts;
        for (const auto& p : a.partitions) {
            if (!parts.empty()) parts += ", ";
            parts += p;
        }

        std::cout << fmt::format(fmt::runtime(hfmt),
            a.gpu, v, std::to_string(a.avail), std::to_string(a.total), parts);
    }

    std::cout << "\n" << theme::color::DIM
              << "  Use 'tccp gpus <type>' for detailed breakdown.\n"
              << "  Use 'tccp gpus info' for GPU guide.\n"
              << theme::color::RESET << "\n";
    return 0;
}

int main(int argc, char** argv) {
    CLI::App app{"tccp — persistent compute shell"};
    app.set_version_flag("--version,-V", TCCP_VERSION);
    app.require_subcommand(0, 1);

    // ── start ─────────────────────────────────────────────
    auto* start_cmd = app.add_subcommand("start", "Start a new session");
    start_cmd->callback([&]() {
        std::cout << theme::banner();
        int rc = run_with_session([](Session& s) {
            auto result = s.start(make_cb());
            if (result.is_err()) {
                std::cerr << theme::error(result.error);
                return 1;
            }
            return 0;
        });
        std::exit(rc);
    });

    // ── shell ─────────────────────────────────────────────
    auto* shell_cmd = app.add_subcommand("shell", "Attach to session shell");
    shell_cmd->callback([&]() {
        int rc = run_with_session([](Session& s) {
            return s.shell();
        });
        std::exit(rc);
    });

    // ── login ─────────────────────────────────────────────
    auto* login_cmd = app.add_subcommand("login", "Open an interactive shell on the login node");
    login_cmd->callback([&]() {
        int rc = run_with_session([](Session& s) {
            return s.login_shell();
        });
        std::exit(rc);
    });

    // ── exec ──────────────────────────────────────────────
    std::vector<std::string> exec_args;
    auto* exec_cmd = app.add_subcommand("exec", "Run a command in the container");
    exec_cmd->add_option("command", exec_args, "Command to run")->required();
    exec_cmd->callback([&]() {
        std::string cmd;
        for (size_t i = 0; i < exec_args.size(); i++) {
            if (i > 0) cmd += " ";
            cmd += exec_args[i];
        }
        int rc = run_with_session([&cmd](Session& s) {
            auto result = s.exec(cmd);
            if (result.is_err()) {
                std::cerr << theme::error(result.error);
                return 1;
            }
            return result.value;
        });
        std::exit(rc);
    });

    // ── sync ──────────────────────────────────────────────
    auto* sync_cmd = app.add_subcommand("sync", "Sync files with compute node");
    sync_cmd->callback([&]() {
        int rc = run_with_session([](Session& s) {
            auto result = s.sync_files(make_cb());
            if (result.is_err()) {
                std::cerr << theme::error(result.error);
                return 1;
            }
            return 0;
        });
        std::exit(rc);
    });

    // ── status ────────────────────────────────────────────
    auto* status_cmd = app.add_subcommand("status", "Show session info");
    status_cmd->callback([&]() {
        int rc = run_with_session([](Session& s) {
            s.status();
            return 0;
        });
        std::exit(rc);
    });

    // ── stop ──────────────────────────────────────────────
    auto* stop_cmd = app.add_subcommand("stop", "Stop the session");
    stop_cmd->callback([&]() {
        int rc = run_with_session([](Session& s) {
            auto result = s.stop(make_cb());
            if (result.is_err()) {
                std::cerr << theme::error(result.error);
                return 1;
            }
            return 0;
        });
        std::exit(rc);
    });

    // ── gpus ──────────────────────────────────────────────
    std::string gpus_arg;
    auto* gpus_cmd = app.add_subcommand("gpus", "Show available GPU resources");
    gpus_cmd->add_option("filter", gpus_arg, "Use 'info' for GPU guide");
    gpus_cmd->callback([&]() {
        if (gpus_arg == "info") {
            show_gpu_guide();
        } else {
            int rc = show_gpu_availability(gpus_arg);
            if (rc != 0) std::exit(rc);
        }
        std::exit(0);
    });

    // ── allocs ────────────────────────────────────────────
    auto* allocs_cmd = app.add_subcommand("allocs", "Show your SLURM allocations");
    allocs_cmd->callback([&]() {
        auto cfg_result = load_config_global_only();
        if (cfg_result.is_err()) {
            std::cerr << theme::error(cfg_result.error);
            std::exit(1);
        }
        auto& g = cfg_result.value;

        SSH ssh(g.host, g.login, g.user, g.password);
        auto conn = ssh.connect();
        if (conn.is_err()) {
            std::cerr << theme::error(conn.error);
            std::exit(1);
        }

        auto result = ssh.run_login(fmt::format(
            "squeue -u {} -h -o '%i|%j|%P|%b|%C|%m|%l|%M|%T|%N' 2>/dev/null", g.user));
        if (!result.ok()) {
            std::cerr << theme::error("Could not query allocations");
            std::exit(1);
        }

        if (trim(result.out).empty()) {
            std::cout << "No active allocations.\n";
            std::exit(0);
        }

        std::cout << "\n";
        std::string hfmt = "  {:<12}{:<20}{:<12}{:<14}{:<12}{:<12}{}\n";
        std::cout << theme::color::DIM
                  << fmt::format(fmt::runtime(hfmt), "JOB ID", "NAME", "PARTITION", "GPU", "TIME USED", "TIME LIMIT", "STATE")
                  << theme::color::RESET;

        std::istringstream iss(result.out);
        std::string line;
        while (std::getline(iss, line)) {
            line = trim(line);
            if (line.empty()) continue;

            std::vector<std::string> f;
            std::istringstream lss(line);
            std::string field;
            while (std::getline(lss, field, '|')) f.push_back(trim(field));
            if (f.size() < 10) continue;

            // Parse GPU from GRES (e.g. "gpu:a100:1")
            std::string gpu_str = f[3];
            auto gpos = gpu_str.find("gpu:");
            if (gpos != std::string::npos) {
                gpu_str = gpu_str.substr(gpos + 4);
            } else if (gpu_str == "(null)") {
                gpu_str = "-";
            }

            std::cout << fmt::format(fmt::runtime(hfmt),
                f[0], f[1], f[2], gpu_str, f[7], f[6], f[8]);
        }

        std::cout << "\n" << theme::color::DIM
                  << "  Use 'tccp dealloc <job-id>' to cancel, or 'tccp dealloc all' to cancel all.\n"
                  << theme::color::RESET << "\n";
        std::exit(0);
    });

    // ── dealloc ──────────────────────────────────────────
    std::string dealloc_arg;
    auto* dealloc_cmd = app.add_subcommand("dealloc", "Cancel SLURM allocations");
    dealloc_cmd->add_option("job", dealloc_arg, "Job ID or 'all'")->required();
    dealloc_cmd->callback([&]() {
        auto cfg_result = load_config_global_only();
        if (cfg_result.is_err()) {
            std::cerr << theme::error(cfg_result.error);
            std::exit(1);
        }
        auto& g = cfg_result.value;

        SSH ssh(g.host, g.login, g.user, g.password);
        auto conn = ssh.connect();
        if (conn.is_err()) {
            std::cerr << theme::error(conn.error);
            std::exit(1);
        }

        std::string cmd;
        bool is_numeric = !dealloc_arg.empty() &&
            std::all_of(dealloc_arg.begin(), dealloc_arg.end(), ::isdigit);

        if (dealloc_arg == "all") {
            cmd = fmt::format("scancel -u {} 2>&1", g.user);
        } else if (is_numeric) {
            cmd = fmt::format("scancel {} 2>&1", dealloc_arg);
        } else {
            cmd = fmt::format("scancel --name {} 2>&1", dealloc_arg);
        }

        auto result = ssh.run_login(cmd);
        if (!result.ok()) {
            std::cerr << theme::error(fmt::format("scancel failed: {}", result.err));
            std::exit(1);
        }

        if (dealloc_arg == "all") {
            std::cout << "All allocations canceled.\n";
        } else {
            std::cout << fmt::format("Allocation {} canceled.\n", dealloc_arg);
        }

        // Clear local session state if it matches
        try {
            auto cfg_full = load_config();
            if (cfg_full.is_ok()) {
                StateStore store(cfg_full.value.project_name);
                if (store.exists()) {
                    auto state = store.load();
                    if (dealloc_arg == "all" || state.slurm_id == dealloc_arg) {
                        store.clear();
                    }
                }
            }
        } catch (...) {}

        std::exit(0);
    });

    // ── setup ─────────────────────────────────────────────
    auto* setup_cmd = app.add_subcommand("setup", "Configure global settings");
    setup_cmd->callback([&]() {
        auto result = run_setup();
        if (result.is_err()) {
            std::cerr << theme::error(result.error);
            std::exit(1);
        }
        std::exit(0);
    });

    // No subcommand → banner + help
    CLI11_PARSE(app, argc, argv);

    if (app.get_subcommands().empty()) {
        std::cout << theme::banner();
        std::cout << app.help();
    }

    return 0;
}
