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
static void do_gpus_detail(BaseCLI& cli, const std::string& gpu_filter) {
    auto& login = cli.service.cluster()->login();

    // Query per-node info: node name, partition, GRES, state, memory, CPUs
    // and also allocated GRES via scontrol or squeue
    auto sinfo_result = login.run(
        "sinfo -N -h -o '%N|%P|%G|%T|%m|%c' 2>/dev/null");
    if (sinfo_result.exit_code != 0 || sinfo_result.stdout_data.empty()) {
        std::cout << theme::error("Failed to query node information.");
        return;
    }

    // Query GPU allocations per node via squeue
    // Format: nodelist|num_gpus (for running jobs)
    std::map<std::string, int> alloc_gpus; // node -> allocated GPU count
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
            std::string gres_str = sqline.substr(sep + 1);

            // Parse GPU count from GRES like "gpu:a100:2" or "gpu:2"
            int gpus = 0;
            std::string gl = gres_str;
            std::transform(gl.begin(), gl.end(), gl.begin(), ::tolower);
            auto gpos = gl.find("gpu:");
            if (gpos != std::string::npos) {
                std::string rest = gres_str.substr(gpos + 4);
                auto colon = rest.find(':');
                if (colon != std::string::npos) {
                    try { gpus = std::stoi(rest.substr(colon + 1)); } catch (...) {}
                } else {
                    try { gpus = std::stoi(rest); } catch (...) {}
                }
            }
            if (gpus <= 0) continue;

            // nodes_str can be a single node or a comma-separated list
            // (not handling bracket notation here — squeue usually expands)
            std::istringstream ns(nodes_str);
            std::string node;
            while (std::getline(ns, node, ',')) {
                if (!node.empty()) alloc_gpus[node] += gpus;
            }
        }
    }

    // Parse sinfo per-node output, filter by GPU type
    struct NodeRow {
        std::string node, partition, gpu_type, state, free_str, mem, cpus;
        int total_gpus = 0;
        int free_gpus = 0;
    };
    std::vector<NodeRow> rows;
    std::set<std::string> seen_nodes; // deduplicate (node appears per partition)

    std::istringstream iss(sinfo_result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::istringstream lss(line);
        std::string field;
        while (std::getline(lss, field, '|')) {
            // trim whitespace
            auto s = field.find_first_not_of(" \t\r\n");
            auto e = field.find_last_not_of(" \t\r\n");
            fields.push_back(s == std::string::npos ? "" : field.substr(s, e - s + 1));
        }
        if (fields.size() < 6) continue;

        std::string gres_str = fields[2];
        std::string gl = gres_str;
        std::transform(gl.begin(), gl.end(), gl.begin(), ::tolower);
        if (gl.find("gpu") == std::string::npos) continue;

        // Parse GPU type and count
        std::string raw_type;
        int gpu_count = 0;
        auto gpos = gl.find("gpu:");
        if (gpos != std::string::npos) {
            std::string rest = gres_str.substr(gpos + 4);
            auto colon = rest.find(':');
            if (colon != std::string::npos) {
                raw_type = rest.substr(0, colon);
                std::transform(raw_type.begin(), raw_type.end(), raw_type.begin(), ::tolower);
                try { gpu_count = std::stoi(rest.substr(colon + 1)); } catch (...) {}
            } else {
                try { gpu_count = std::stoi(rest); } catch (...) {}
            }
        }
        if (gpu_count <= 0) continue;

        // Resolve variant-aware type
        std::string display_type = raw_type;
        std::string node_name = fields[0];
        std::string node_lower = node_name;
        std::transform(node_lower.begin(), node_lower.end(), node_lower.begin(), ::tolower);
        auto variants = find_variants_by_base(raw_type);
        for (const auto* v : variants) {
            std::string pfx = v->node_prefix;
            std::transform(pfx.begin(), pfx.end(), pfx.begin(), ::tolower);
            if (node_lower.find(pfx) == 0) {
                display_type = v->id;
                break;
            }
        }

        // Filter: does this node's GPU type match the requested filter?
        std::string filter_lower = gpu_filter;
        std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
        if (display_type != filter_lower && raw_type != filter_lower) continue;

        // Deduplicate (a node can appear in multiple partitions)
        if (seen_nodes.count(node_name)) continue;
        seen_nodes.insert(node_name);

        std::string partition = fields[1];
        if (!partition.empty() && partition.back() == '*') partition.pop_back();

        std::string state = fields[3];
        std::transform(state.begin(), state.end(), state.begin(), ::tolower);

        int allocated = alloc_gpus.count(node_name) ? alloc_gpus[node_name] : 0;
        int free = std::max(0, gpu_count - allocated);

        NodeRow r;
        r.node = node_name;
        r.partition = partition;
        r.gpu_type = display_type;
        r.total_gpus = gpu_count;
        r.free_gpus = free;
        r.free_str = fmt::format("{}/{}", free, gpu_count);
        int mem_mb = 0;
        try { mem_mb = std::stoi(fields[4]); } catch (...) {}
        r.mem = mem_mb > 0 ? fmt::format("{}G", mem_mb / 1024) : "?";
        r.cpus = fields[5];

        // Normalize state for display
        if (state.find("idle") != std::string::npos) r.state = "idle";
        else if (state.find("mix") != std::string::npos) r.state = "mixed";
        else if (state.find("alloc") != std::string::npos) r.state = "full";
        else if (state.find("drain") != std::string::npos) r.state = "drain";
        else if (state.find("down") != std::string::npos) r.state = "down";
        else r.state = state;

        rows.push_back(std::move(r));
    }

    if (rows.empty()) {
        std::cout << theme::error(fmt::format("No nodes found with GPU type '{}'", gpu_filter));
        std::cout << theme::dim("    Run 'gpus' without arguments to see available types.") << "\n";
        return;
    }

    // Sort: free GPUs descending, then node name
    std::sort(rows.begin(), rows.end(), [](const NodeRow& a, const NodeRow& b) {
        if (a.free_gpus != b.free_gpus) return a.free_gpus > b.free_gpus;
        return a.node < b.node;
    });

    // Compute totals
    int sum_free = 0, sum_total = 0;
    for (const auto& r : rows) {
        sum_free += r.free_gpus;
        sum_total += r.total_gpus;
    }

    // Column widths
    size_t nw = 4, pw = 9, sw = 5, fw = 9, mw = 3, cw = 4;
    for (const auto& r : rows) {
        nw = std::max(nw, r.node.size());
        pw = std::max(pw, r.partition.size());
        sw = std::max(sw, r.state.size());
        fw = std::max(fw, r.free_str.size());
        mw = std::max(mw, r.mem.size());
        cw = std::max(cw, r.cpus.size());
    }

    std::string rfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    nw + 2, pw + 2, sw + 2, fw + 2, mw + 2);
    std::cout << "\n";
    std::cout << theme::dim(fmt::format("    {} — {}/{} GPUs free across {} nodes",
                                         gpu_filter, sum_free, sum_total, rows.size())) << "\n\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(rfmt), "NODE", "PARTITION", "STATE", "FREE/TOTAL", "MEM", "CPUS")
              << theme::color::RESET;

    for (const auto& r : rows) {
        // Color the free/total based on availability
        std::string state_str;
        if (r.state == "idle") state_str = theme::color::GREEN + r.state + theme::color::RESET;
        else if (r.state == "mixed") state_str = theme::color::YELLOW + r.state + theme::color::RESET;
        else if (r.state == "down" || r.state == "drain") state_str = theme::color::RED + r.state + theme::color::RESET;
        else state_str = theme::color::DIM + r.state + theme::color::RESET;

        std::string free_str;
        if (r.free_gpus == r.total_gpus) free_str = theme::color::GREEN + r.free_str + theme::color::RESET;
        else if (r.free_gpus > 0) free_str = theme::color::YELLOW + r.free_str + theme::color::RESET;
        else free_str = theme::color::DIM + r.free_str + theme::color::RESET;

        // Manual padding since ANSI codes break fmt width
        std::string pad_fmt = fmt::format("  {{:<{}}} {{:<{}}} ",
                                           nw + 2, pw + 2);
        std::cout << fmt::format(fmt::runtime(pad_fmt), r.node, r.partition)
                  << state_str << std::string(sw + 2 - r.state.size(), ' ')
                  << free_str << std::string(fw + 2 - r.free_str.size(), ' ')
                  << fmt::format("{:<{}} {}\n", r.mem, mw + 2, r.cpus);
    }
    std::cout << "\n";
}

static void do_gpus(BaseCLI& cli, const std::string& arg) {
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
