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

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<10} {:<14} {:<10} {:<10} {:<8} {}",
                             "SLURM ID", "NODE", "PARTITION", "GPU", "STATUS", "REMAINING")
              << theme::color::RESET << "\n";

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

        std::cout << fmt::format("  {:<10} {:<14} {:<10} {:<10} ", s.slurm_id, s.node, s.partition, s.gpu)
                  << status_str
                  << std::string(8 - s.status.size(), ' ')
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

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<16} {:<14} {:<8} {:<8} {:<10} {:<10} {}",
                             "PARTITION", "GPU", "x/NODE", "AVAIL", "TOTAL", "MEM/NODE", "CPUS")
              << theme::color::RESET << "\n";

    for (const auto& r : resources) {
        bool accessible = !has_filter || allowed.count(r.partition);
        std::string mem_display = r.mem_mb > 0
            ? fmt::format("{}G", r.mem_mb / 1024)
            : "?";

        std::string line_str = fmt::format("  {:<16} {:<14} {:<8} {:<8} {:<10} {:<10} {}\n",
                                  r.partition,
                                  r.gpu_type.empty() ? "gpu" : r.gpu_type,
                                  r.gpu_per_node,
                                  r.avail_nodes,
                                  r.total_nodes,
                                  mem_display,
                                  r.cpus_per_node);

        if (!accessible) {
            std::cout << theme::color::DIM << line_str << theme::color::RESET;
        } else {
            std::cout << line_str;
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
