#include "../base_cli.hpp"
#include "../theme.hpp"
#include <core/utils.hpp>
#include <managers/gpu_discovery.hpp>
#include <iostream>
#include <sstream>
#include <set>
#include <map>
#include <algorithm>
#include <ctime>
#include <fmt/format.h>

static void do_allocs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.alloc_manager()) return;

    auto summaries = cli.service.list_allocations();
    if (summaries.empty()) {
        std::cout << theme::dim("  No managed allocations.") << "\n";
        return;
    }

    // Compute column widths from headers and data
    size_t w0 = 8, w1 = 4, w2 = 9, w3 = 3, w4 = 6, w5 = 9; // header lengths
    for (const auto& s : summaries) {
        w0 = std::max(w0, s.slurm_id.size());
        w1 = std::max(w1, s.node.size());
        w2 = std::max(w2, s.partition.size());
        w3 = std::max(w3, s.gpu.size());
        w4 = std::max(w4, s.status.size());
        w5 = std::max(w5, s.remaining.size());
    }

    std::string hfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    w0 + 2, w1 + 2, w2 + 2, w3 + 2, w4 + 2);

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(hfmt), "SLURM ID", "NODE", "PARTITION", "GPU", "STATUS", "REMAINING")
              << theme::color::RESET;

    for (const auto& s : summaries) {
        // Color the status
        std::string status_str;
        if (s.status == "IDLE") {
            status_str = theme::color::GREEN + s.status + theme::color::RESET;
        } else if (s.status == "BUSY") {
            status_str = theme::color::YELLOW + s.status + theme::color::RESET;
        } else {
            status_str = theme::color::DIM + s.status + theme::color::RESET;
        }

        // Pad manually for status since it has ANSI codes
        std::string sfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} ",
                                        w0 + 2, w1 + 2, w2 + 2, w3 + 2);
        std::cout << fmt::format(fmt::runtime(sfmt), s.slurm_id, s.node, s.partition, s.gpu)
                  << status_str
                  << std::string(w4 + 2 - s.status.size(), ' ')
                  << s.remaining << "\n";
    }
    std::cout << "\n";
}

static void do_dealloc(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.service.alloc_manager()) return;

    auto cb = [](const std::string& msg) {
        std::cout << theme::dim(msg);
    };

    if (arg.empty() || arg == "all") {
        cli.service.deallocate("", cb);
    } else {
        cli.service.deallocate(arg, cb);
    }
}

// Per-node GPU detail for a specific GPU type
// Parse GPU count from GRES string like "gpu:a100:2" or "gpu:2"
static int parse_gpu_count(const std::string& gres_str) {
    std::string gl = gres_str;
    std::transform(gl.begin(), gl.end(), gl.begin(), ::tolower);
    auto gpos = gl.find("gpu:");
    if (gpos == std::string::npos) return 0;
    std::string rest = gres_str.substr(gpos + 4);
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        try { return std::stoi(rest.substr(colon + 1)); } catch (...) {}
    } else {
        try { return std::stoi(rest); } catch (...) {}
    }
    return 0;
}

// Parse raw GPU type from GRES string
static std::string parse_gpu_type(const std::string& gres_str) {
    std::string gl = gres_str;
    std::transform(gl.begin(), gl.end(), gl.begin(), ::tolower);
    auto gpos = gl.find("gpu:");
    if (gpos == std::string::npos) return "";
    std::string rest = gres_str.substr(gpos + 4);
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        std::string t = rest.substr(0, colon);
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        return t;
    }
    return "";
}

// Trim whitespace from a field
static std::string trim_field(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Resolve display type using variant node prefix matching
static std::string resolve_display_type(const std::string& raw_type,
                                         const std::string& node_name) {
    std::string node_lower = node_name;
    std::transform(node_lower.begin(), node_lower.end(), node_lower.begin(), ::tolower);
    auto variants = find_variants_by_base(raw_type);
    for (const auto* v : variants) {
        std::string pfx = v->node_prefix;
        std::transform(pfx.begin(), pfx.end(), pfx.begin(), ::tolower);
        if (node_lower.find(pfx) == 0) return v->id;
    }
    return raw_type;
}

static void do_gpus_detail(BaseCLI& cli, const std::string& gpu_filter) {
    auto& login = cli.service.cluster()->login();

    // Query per-node info: node, partition, GRES, state, total mem, free mem, CPUs
    auto sinfo_result = login.run(
        "sinfo -N -h -o '%N|%P|%G|%T|%m|%e|%c' 2>/dev/null");
    if (sinfo_result.exit_code != 0 || sinfo_result.stdout_data.empty()) {
        std::cout << theme::error("Failed to query node information.");
        return;
    }

    // Query GPU allocations per node via squeue (running jobs)
    std::map<std::string, int> alloc_gpus;
    auto squeue_result = login.run(
        "squeue -h -t RUNNING -o '%N|%b' 2>/dev/null");
    if (squeue_result.exit_code == 0 && !squeue_result.stdout_data.empty()) {
        std::istringstream sq(squeue_result.stdout_data);
        std::string sqline;
        while (std::getline(sq, sqline)) {
            if (sqline.empty()) continue;
            auto sep = sqline.find('|');
            if (sep == std::string::npos) continue;
            std::string nodes_str = sqline.substr(0, sep);
            int gpus = parse_gpu_count(sqline.substr(sep + 1));
            if (gpus <= 0) continue;
            std::istringstream ns(nodes_str);
            std::string node;
            while (std::getline(ns, node, ',')) {
                if (!node.empty()) alloc_gpus[node] += gpus;
            }
        }
    }

    // Count pending jobs for this GPU type
    int pending_jobs = 0;
    auto pending_result = login.run(
        "squeue -h -t PENDING -o '%b' 2>/dev/null");
    if (pending_result.exit_code == 0 && !pending_result.stdout_data.empty()) {
        std::string filter_lower = gpu_filter;
        std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
        std::istringstream pq(pending_result.stdout_data);
        std::string pline;
        while (std::getline(pq, pline)) {
            std::string pl = pline;
            std::transform(pl.begin(), pl.end(), pl.begin(), ::tolower);
            if (pl.find("gpu:") != std::string::npos) {
                // Check if the GPU type matches
                std::string ptype = parse_gpu_type(pline);
                if (ptype == filter_lower || ptype.empty()) pending_jobs++;
            }
        }
    }

    // Parse sinfo output, filter by GPU type
    struct NodeRow {
        std::string node, partition, state;
        int total_gpus = 0, free_gpus = 0;
        int mem_total_gb = 0, mem_free_gb = 0;
        int cpus = 0;
        bool usable = true; // not down/drain
    };
    std::vector<NodeRow> rows;
    std::set<std::string> seen_nodes;
    std::string filter_lower = gpu_filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);

    std::istringstream iss(sinfo_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::istringstream lss(line);
        std::string field;
        while (std::getline(lss, field, '|')) fields.push_back(trim_field(field));
        if (fields.size() < 7) continue;

        std::string gres_lower = fields[2];
        std::transform(gres_lower.begin(), gres_lower.end(), gres_lower.begin(), ::tolower);
        if (gres_lower.find("gpu") == std::string::npos) continue;

        std::string raw_type = parse_gpu_type(fields[2]);
        int gpu_count = parse_gpu_count(fields[2]);
        if (gpu_count <= 0) continue;

        std::string node_name = fields[0];
        std::string display_type = resolve_display_type(raw_type, node_name);
        if (display_type != filter_lower && raw_type != filter_lower) continue;

        if (seen_nodes.count(node_name)) continue;
        seen_nodes.insert(node_name);

        std::string partition = fields[1];
        if (!partition.empty() && partition.back() == '*') partition.pop_back();

        std::string state = fields[3];
        std::transform(state.begin(), state.end(), state.begin(), ::tolower);

        int allocated = alloc_gpus.count(node_name) ? alloc_gpus[node_name] : 0;

        NodeRow r;
        r.node = node_name;
        r.partition = partition;
        r.total_gpus = gpu_count;
        r.cpus = 0;
        try { r.cpus = std::stoi(fields[6]); } catch (...) {}

        int mem_total = 0, mem_free = 0;
        try { mem_total = std::stoi(fields[4]); } catch (...) {}
        try { mem_free = std::stoi(fields[5]); } catch (...) {}
        r.mem_total_gb = mem_total / 1024;
        r.mem_free_gb = mem_free / 1024;

        if (state.find("idle") != std::string::npos) {
            r.state = "idle";
            r.free_gpus = gpu_count;
        } else if (state.find("mix") != std::string::npos) {
            r.state = "mixed";
            r.free_gpus = std::max(0, gpu_count - allocated);
        } else if (state.find("alloc") != std::string::npos) {
            r.state = "full";
            r.free_gpus = 0;
        } else if (state.find("drain") != std::string::npos) {
            r.state = "drain";
            r.free_gpus = 0;
            r.usable = false;
        } else if (state.find("down") != std::string::npos) {
            r.state = "down";
            r.free_gpus = 0;
            r.usable = false;
        } else {
            r.state = state;
            r.free_gpus = std::max(0, gpu_count - allocated);
        }

        rows.push_back(std::move(r));
    }

    if (rows.empty()) {
        std::cout << theme::error(fmt::format("No nodes found with GPU type '{}'", gpu_filter));
        std::cout << theme::dim("    Run 'gpus' without arguments to see available types.") << "\n";
        return;
    }

    // Sort: usable first, then free GPUs descending, then node name
    std::sort(rows.begin(), rows.end(), [](const NodeRow& a, const NodeRow& b) {
        if (a.usable != b.usable) return a.usable > b.usable;
        if (a.free_gpus != b.free_gpus) return a.free_gpus > b.free_gpus;
        return a.node < b.node;
    });

    // Compute totals
    int gpus_free = 0, gpus_busy = 0, gpus_down = 0, total_gpus = 0;
    int nodes_up = 0, nodes_down = 0;
    for (const auto& r : rows) {
        total_gpus += r.total_gpus;
        if (r.usable) {
            gpus_free += r.free_gpus;
            gpus_busy += r.total_gpus - r.free_gpus;
            nodes_up++;
        } else {
            gpus_down += r.total_gpus;
            nodes_down++;
        }
    }

    // Summary
    std::cout << "\n";
    std::string summary = fmt::format("    {} — ", gpu_filter);
    if (gpus_free > 0)
        summary += theme::color::GREEN + fmt::format("{} free", gpus_free) + theme::color::RESET;
    else
        summary += theme::color::DIM + "0 free" + theme::color::RESET;
    summary += fmt::format(", {} in use", gpus_busy);
    if (gpus_down > 0)
        summary += ", " + theme::color::RED + fmt::format("{} offline", gpus_down) + theme::color::RESET;
    summary += fmt::format("  ({} GPUs across {} nodes", total_gpus, rows.size());
    if (nodes_down > 0)
        summary += fmt::format(", {} down", nodes_down);
    summary += ")";
    std::cout << summary << "\n";
    if (pending_jobs > 0) {
        std::cout << theme::color::YELLOW
                  << fmt::format("    {} pending job{} in queue", pending_jobs, pending_jobs == 1 ? "" : "s")
                  << theme::color::RESET << "\n";
    }
    std::cout << "\n";

    // Column widths — compute from actual data to avoid truncation
    size_t nw = 4, pw = 9, sw = 5, gw = 4, mw = 6, cw = 4;
    for (const auto& r : rows) {
        nw = std::max(nw, r.node.size());
        pw = std::max(pw, r.partition.size());
        sw = std::max(sw, r.state.size());
        std::string gs = fmt::format("{}/{}", r.free_gpus, r.total_gpus);
        gw = std::max(gw, gs.size());
        std::string ms = fmt::format("{}G/{}G", r.mem_free_gb, r.mem_total_gb);
        mw = std::max(mw, ms.size());
    }

    // Header
    std::string hfmt = fmt::format("    {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    nw + 2, pw + 2, sw + 2, gw + 2, mw + 2);
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(hfmt), "NODE", "PARTITION", "STATE", "GPUs", "MEMORY", "CPUs")
              << theme::color::RESET;

    for (const auto& r : rows) {
        // State coloring
        std::string state_str;
        if (r.state == "idle") state_str = theme::color::GREEN + r.state + theme::color::RESET;
        else if (r.state == "mixed") state_str = theme::color::YELLOW + r.state + theme::color::RESET;
        else if (r.state == "down" || r.state == "drain") state_str = theme::color::RED + r.state + theme::color::RESET;
        else state_str = theme::color::DIM + r.state + theme::color::RESET;

        // GPU free/total
        std::string gpu_str = fmt::format("{}/{}", r.free_gpus, r.total_gpus);
        std::string gpu_colored;
        if (!r.usable) gpu_colored = theme::color::DIM + gpu_str + theme::color::RESET;
        else if (r.free_gpus == r.total_gpus) gpu_colored = theme::color::GREEN + gpu_str + theme::color::RESET;
        else if (r.free_gpus > 0) gpu_colored = theme::color::YELLOW + gpu_str + theme::color::RESET;
        else gpu_colored = theme::color::DIM + gpu_str + theme::color::RESET;

        // Memory: show free/total
        std::string mem_str = fmt::format("{}G/{}G", r.mem_free_gb, r.mem_total_gb);
        std::string mem_colored;
        if (!r.usable) mem_colored = theme::color::DIM + mem_str + theme::color::RESET;
        else if (r.mem_free_gb > r.mem_total_gb / 2) mem_colored = mem_str;
        else mem_colored = theme::color::YELLOW + mem_str + theme::color::RESET;

        // Dim entire row for offline nodes
        std::string pad_fmt = fmt::format("    {{:<{}}} {{:<{}}} ", nw + 2, pw + 2);
        if (!r.usable) {
            std::cout << theme::color::DIM
                      << fmt::format(fmt::runtime(pad_fmt), r.node, r.partition)
                      << theme::color::RESET;
        } else {
            std::cout << fmt::format(fmt::runtime(pad_fmt), r.node, r.partition);
        }

        std::cout << state_str << std::string(sw + 2 - r.state.size(), ' ')
                  << gpu_colored << std::string(gw + 2 - gpu_str.size(), ' ')
                  << mem_colored << std::string(mw + 2 - mem_str.size(), ' ')
                  << r.cpus << "\n";
    }
    std::cout << "\n";
}

static void do_gpus_info() {
    std::cout << "\n";
    std::cout << theme::color::BOLD << "  GPU Guide" << theme::color::RESET
              << theme::color::DIM << " (least to most powerful)" << theme::color::RESET << "\n\n";

    // Each entry: name, VRAM, best for, notes
    struct GpuInfo {
        std::string name, vram, use_case, notes;
    };
    std::vector<GpuInfo> gpus = {
        {"t4",          "16 GB",  "Inference, light training",
         "Oldest Turing card. Slow FP32 but decent FP16. Good for small models and debugging."},
        {"p100",        "16 GB",  "Legacy workloads",
         "Pascal generation. No tensor cores. Mainly useful when nothing else is free."},
        {"rtx_6000",    "24 GB",  "Medium training",
         "Turing with tensor cores. Decent for mid-size models."},
        {"v100",        "32 GB",  "General training",
         "Volta. First-gen tensor cores. Solid all-rounder, good FP16 throughput."},
        {"a100-40gb",   "40 GB",  "Large model training",
         "Ampere. 3rd-gen tensor cores, TF32, BF16. Fast interconnect. Great price/perf."},
        {"rtx_a5000",   "24 GB",  "Medium training, multi-GPU",
         "Ampere workstation GPU. 8 per node — good for data-parallel training."},
        {"l40",         "48 GB",  "Large models, fine-tuning",
         "Ada Lovelace. Good VRAM for big batch sizes or large models."},
        {"rtx_a6000",   "48 GB",  "Large models",
         "Ampere workstation. 48 GB VRAM, 8 per node."},
        {"rtx_6000ada", "48 GB",  "Large models, fast training",
         "Ada Lovelace workstation. Newer arch than rtx_a6000."},
        {"a100-80gb",   "80 GB",  "LLMs, very large models",
         "The workhorse. 80 GB VRAM handles most LLM fine-tuning. High bandwidth."},
        {"l40s",        "48 GB",  "Fast training, inference",
         "Ada Lovelace server GPU. Better FP8 than L40. Newer generation."},
        {"h100",        "80 GB",  "Largest models, fastest training",
         "Hopper. 4th-gen tensor cores, FP8, transformer engine. Top of the line."},
    };

    for (const auto& g : gpus) {
        std::cout << "  " << theme::color::BOLD << fmt::format("{:<14}", g.name)
                  << theme::color::RESET
                  << theme::color::DIM << fmt::format("{:>6} VRAM", g.vram) << theme::color::RESET
                  << "  " << g.use_case << "\n"
                  << theme::color::DIM << fmt::format("  {:<14}{}\n", "", g.notes)
                  << theme::color::RESET;
    }

    std::cout << "\n"
              << theme::color::DIM
              << "  Partitions: 'gpu' is dedicated (guaranteed). 'preempt' is shared (may be interrupted).\n"
              << "  Use 'gpus' to see live availability. Use 'gpus <type>' for per-node detail.\n"
              << "  Config: gpu: a100-80gb  gpu_count: 1  partition: preempt\n"
              << theme::color::RESET << "\n";
}

static void do_gpus(BaseCLI& cli, const std::string& arg) {
    // 'gpus info' doesn't need a connection
    if (arg == "info") {
        do_gpus_info();
        return;
    }

    if (!cli.require_connection()) return;

    // If a GPU type is specified, show per-node detail
    if (!arg.empty()) {
        do_gpus_detail(cli, arg);
        return;
    }

    auto resources = discover_gpu_resources(cli.service.cluster()->login());

    if (resources.empty()) {
        std::cout << "No GPU resources found on this cluster.\n";
        return;
    }

    // Check user permissions
    auto username = get_cluster_username();
    auto user_parts = discover_user_partitions(cli.service.cluster()->login(), username);
    std::set<std::string> allowed(user_parts.begin(), user_parts.end());
    bool has_filter = !allowed.empty();

    // Build display data and compute column widths
    struct GRow {
        std::string part, gpu, per_node, avail_gpus, total_gpus, nodes, mem, cpus;
        bool accessible;
    };
    std::vector<GRow> grows;
    grows.reserve(resources.size());
    for (const auto& r : resources) {
        GRow g;
        g.part = r.partition;
        g.gpu = r.gpu_type.empty() ? "gpu" : r.gpu_type;
        g.per_node = std::to_string(r.gpu_per_node);
        g.avail_gpus = std::to_string(r.avail_nodes * r.gpu_per_node);
        g.total_gpus = std::to_string(r.total_nodes * r.gpu_per_node);
        g.nodes = fmt::format("{}/{}", r.avail_nodes, r.total_nodes);
        g.mem = r.mem_mb > 0 ? fmt::format("{}G", r.mem_mb / 1024) : "?";
        g.cpus = std::to_string(r.cpus_per_node);
        g.accessible = !has_filter || allowed.count(r.partition);
        grows.push_back(std::move(g));
    }

    size_t w0 = 9, w1 = 3, w2 = 8, w3 = 5, w4 = 5, w5 = 5, w6 = 3, w7 = 4;
    for (const auto& g : grows) {
        w0 = std::max(w0, g.part.size());
        w1 = std::max(w1, g.gpu.size());
        w2 = std::max(w2, g.per_node.size());
        w3 = std::max(w3, g.avail_gpus.size());
        w4 = std::max(w4, g.total_gpus.size());
        w5 = std::max(w5, g.nodes.size());
        w6 = std::max(w6, g.mem.size());
        w7 = std::max(w7, g.cpus.size());
    }

    std::string gfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    w0 + 2, w1 + 2, w2 + 2, w3 + 2, w4 + 2, w5 + 2, w6 + 2);
    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(gfmt), "PARTITION", "GPU", "GPU/NODE", "AVAIL", "TOTAL", "NODES", "MEM", "CPUS")
              << theme::color::RESET;

    for (const auto& g : grows) {
        std::string line = fmt::format(fmt::runtime(gfmt),
            g.part, g.gpu, g.per_node, g.avail_gpus, g.total_gpus, g.nodes, g.mem, g.cpus);
        if (!g.accessible) {
            std::cout << theme::color::DIM << line << theme::color::RESET;
        } else {
            std::cout << line;
        }
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << "  AVAIL/TOTAL = GPUs on idle/mix nodes.  NODES = idle+mix / total.\n"
              << "  Use 'gpus <type>' for per-node breakdown (e.g. 'gpus a100-80gb').\n"
              << "  Config:  gpu: <type>  gpu_count: <n>  partition: <name>"
              << theme::color::RESET << "\n\n";
}

void register_allocations_commands(BaseCLI& cli) {
    cli.add_command("allocs", do_allocs, "List compute allocations");
    cli.add_command("dealloc", do_dealloc, "Release allocations");
    cli.add_command("gpus", do_gpus, "List available GPU resources");
}
