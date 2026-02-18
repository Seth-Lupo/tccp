#include "../base_cli.hpp"
#include "../theme.hpp"
#include <core/utils.hpp>
#include <managers/gpu_discovery.hpp>
#include <iostream>
#include <set>
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

static void do_gpus(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;

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
        std::string part, gpu, xnode, avail, total, mem, cpus;
        bool accessible;
    };
    std::vector<GRow> grows;
    grows.reserve(resources.size());
    for (const auto& r : resources) {
        GRow g;
        g.part = r.partition;
        g.gpu = r.gpu_type.empty() ? "gpu" : r.gpu_type;
        g.xnode = std::to_string(r.gpu_per_node);
        g.avail = std::to_string(r.avail_nodes);
        g.total = std::to_string(r.total_nodes);
        g.mem = r.mem_mb > 0 ? fmt::format("{}G", r.mem_mb / 1024) : "?";
        g.cpus = std::to_string(r.cpus_per_node);
        g.accessible = !has_filter || allowed.count(r.partition);
        grows.push_back(std::move(g));
    }

    size_t gw0 = 9, gw1 = 3, gw2 = 6, gw3 = 5, gw4 = 5, gw5 = 8, gw6 = 4;
    for (const auto& g : grows) {
        gw0 = std::max(gw0, g.part.size());
        gw1 = std::max(gw1, g.gpu.size());
        gw2 = std::max(gw2, g.xnode.size());
        gw3 = std::max(gw3, g.avail.size());
        gw4 = std::max(gw4, g.total.size());
        gw5 = std::max(gw5, g.mem.size());
        gw6 = std::max(gw6, g.cpus.size());
    }

    std::string gfmt = fmt::format("  {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{:<{}}} {{}}\n",
                                    gw0 + 2, gw1 + 2, gw2 + 2, gw3 + 2, gw4 + 2, gw5 + 2);
    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format(fmt::runtime(gfmt), "PARTITION", "GPU", "x/NODE", "AVAIL", "TOTAL", "MEM/NODE", "CPUS")
              << theme::color::RESET;

    for (const auto& g : grows) {
        std::string line = fmt::format(fmt::runtime(gfmt),
            g.part, g.gpu, g.xnode, g.avail, g.total, g.mem, g.cpus);
        if (!g.accessible) {
            std::cout << theme::color::DIM << line << theme::color::RESET;
        } else {
            std::cout << line;
        }
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << "  Partition is auto-selected when gpu_type is set in tccp.yaml.\n"
              << "  Config:  slurm: { gpu_type: <name>, gpu_count: <n> }"
              << theme::color::RESET << "\n\n";
}

void register_allocations_commands(BaseCLI& cli) {
    cli.add_command("allocs", do_allocs, "List compute allocations");
    cli.add_command("dealloc", do_dealloc, "Release allocations");
    cli.add_command("gpus", do_gpus, "List available GPU resources");
}
