#include "../base_cli.hpp"
#include "../theme.hpp"
#include <core/credentials.hpp>
#include <managers/gpu_discovery.hpp>
#include <iostream>
#include <set>
#include <ctime>
#include <fmt/format.h>

static std::string get_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "";
}

static void do_allocs(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.allocs) return;

    const auto& allocs = cli.allocs->allocations();
    if (allocs.empty()) {
        std::cout << theme::dim("  No managed allocations.") << "\n";
        return;
    }

    std::cout << "\n";
    std::cout << theme::color::DIM
              << fmt::format("  {:<10} {:<14} {:<10} {:<10} {:<8} {}",
                             "SLURM ID", "NODE", "PARTITION", "GPU", "STATUS", "REMAINING")
              << theme::color::RESET << "\n";

    for (const auto& a : allocs) {
        std::string status;
        if (a.node.empty()) {
            status = "PENDING";
        } else if (a.active_job_id.empty()) {
            status = "IDLE";
        } else {
            status = "BUSY";
        }

        // GPU info
        std::string gpu_str = "-";
        if (!a.gpu_type.empty() || a.gpu_count > 0) {
            gpu_str = fmt::format("{}:{}", a.gpu_type.empty() ? "gpu" : a.gpu_type,
                                  a.gpu_count > 0 ? a.gpu_count : 1);
        }

        // Remaining time
        std::string remaining = "-";
        if (!a.start_time.empty()) {
            struct tm tm_buf = {};
            if (sscanf(a.start_time.c_str(), "%d-%d-%dT%d:%d:%d",
                       &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
                       &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec) == 6) {
                tm_buf.tm_year -= 1900;
                tm_buf.tm_mon -= 1;
                auto start = mktime(&tm_buf);
                if (start > 0) {
                    auto now = std::time(nullptr);
                    int elapsed_min = static_cast<int>(std::difftime(now, start) / 60.0);
                    int rem = a.duration_minutes - elapsed_min;
                    if (rem < 0) rem = 0;
                    int h = rem / 60, m = rem % 60;
                    remaining = fmt::format("{}h {:02d}m", h, m);
                }
            }
        }

        std::string partition = a.partition.empty() ? "batch" : a.partition;
        std::string node = a.node.empty() ? "(waiting)" : a.node;

        // Color the status
        std::string status_str;
        if (status == "IDLE") {
            status_str = theme::color::GREEN + status + theme::color::RESET;
        } else if (status == "BUSY") {
            status_str = theme::color::YELLOW + status + theme::color::RESET;
        } else {
            status_str = theme::color::DIM + status + theme::color::RESET;
        }

        std::cout << fmt::format("  {:<10} {:<14} {:<10} {:<10} ", a.slurm_id, node, partition, gpu_str)
                  << status_str
                  << std::string(8 - status.size(), ' ')
                  << remaining << "\n";
    }
    std::cout << "\n";
}

static void do_dealloc(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (!cli.allocs) return;

    auto cb = [](const std::string& msg) {
        std::cout << theme::dim(msg);
    };

    if (arg.empty() || arg == "all") {
        cli.allocs->deallocate_all_idle(cb);
    } else {
        cli.allocs->deallocate(arg, cb);
    }
}

static void do_gpus(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;

    auto resources = discover_gpu_resources(cli.cluster->login());

    if (resources.empty()) {
        std::cout << "No GPU resources found on this cluster.\n";
        return;
    }

    // Check user permissions
    auto username = get_username();
    auto user_parts = discover_user_partitions(cli.cluster->login(), username);
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
