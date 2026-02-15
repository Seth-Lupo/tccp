#include "gpu_discovery.hpp"
#include <sstream>
#include <algorithm>
#include <set>
#include <fstream>
#include <fmt/format.h>

// ── Debug logging ────────────────────────────────────────────

static void gpu_log(const std::string& msg) {
    std::ofstream out("/tmp/tccp_debug.log", std::ios::app);
    if (out) out << "[gpu] " << msg << "\n";
}

// ── Parsing helpers ──────────────────────────────────────────

// Parse GRES string like "gpu:a100:4" or "gpu:4" into type + count
static void parse_gres(const std::string& gres,
                       std::string& out_type, int& out_count) {
    out_type.clear();
    out_count = 0;

    // Find "gpu:" prefix (case-insensitive search)
    std::string lower = gres;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto pos = lower.find("gpu:");
    if (pos == std::string::npos) return;

    // Use original case for the type name extraction
    std::string rest = gres.substr(pos + 4);

    // Could be "a100:4" (type:count) or just "4" (count only)
    auto colon = rest.find(':');
    if (colon != std::string::npos) {
        out_type = rest.substr(0, colon);
        try { out_count = std::stoi(rest.substr(colon + 1)); } catch (...) {}
    } else {
        try {
            out_count = std::stoi(rest);
        } catch (...) {
            out_type = rest;
            out_count = 1;
        }
    }

    // Normalize type to lowercase for consistent matching
    std::transform(out_type.begin(), out_type.end(), out_type.begin(), ::tolower);
}

static int parse_int(const std::string& s, int fallback = 0) {
    try { return std::stoi(s); } catch (...) { return fallback; }
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n'\"");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n'\"");
    return s.substr(start, end - start + 1);
}

// Case-insensitive match with word-boundary awareness.
// "a100" matches "a100", "a100-sxm4-80gb", "nvidia_a100"
// "a10" does NOT match "a100" (no boundary after "a10" in "a100")
static bool is_boundary(char c) {
    return c == '-' || c == '_' || c == '.' || c == ':';
}

static bool type_matches(const std::string& resource_type,
                          const std::string& requested_type) {
    if (requested_type.empty()) return true;

    // Normalize both to lowercase
    std::string r = resource_type;
    std::string q = requested_type;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // Exact match
    if (r == q) return true;

    // Substring match with word boundary: the character after the match
    // must be end-of-string or a separator (-, _, ., :)
    auto pos = r.find(q);
    while (pos != std::string::npos) {
        size_t end = pos + q.size();
        bool boundary_after = (end == r.size() || is_boundary(r[end]));
        bool boundary_before = (pos == 0 || is_boundary(r[pos - 1]));
        if (boundary_after && boundary_before) return true;
        pos = r.find(q, pos + 1);
    }

    return false;
}

// ── Query: GPU resources ─────────────────────────────────────

std::vector<GpuResource> discover_gpu_resources(SSHConnection& login) {
    auto result = login.run(
        "sinfo -h -o '%P|%G|%D|%m|%c|%T' 2>/dev/null");

    gpu_log(fmt::format("sinfo exit={} stdout_len={}", result.exit_code,
                        result.stdout_data.size()));
    if (!result.stdout_data.empty()) {
        gpu_log("sinfo output (first 1000): " + result.stdout_data.substr(0, 1000));
    }

    std::vector<GpuResource> resources;
    if (result.exit_code != 0 || result.stdout_data.empty()) {
        return resources;
    }

    std::istringstream iss(result.stdout_data);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;

        std::vector<std::string> fields;
        std::istringstream lss(line);
        std::string field;
        while (std::getline(lss, field, '|')) {
            fields.push_back(trim(field));
        }
        if (fields.size() < 6) continue;

        std::string gres_str = fields[1];
        // Check for GPU in GRES (case-insensitive)
        std::string gres_lower = gres_str;
        std::transform(gres_lower.begin(), gres_lower.end(), gres_lower.begin(), ::tolower);
        if (gres_lower.find("gpu") == std::string::npos) continue;

        std::string gpu_type;
        int gpu_count;
        parse_gres(gres_str, gpu_type, gpu_count);
        if (gpu_count <= 0) continue;

        std::string partition = fields[0];
        if (!partition.empty() && partition.back() == '*')
            partition.pop_back();

        std::string state = fields[5];
        std::transform(state.begin(), state.end(), state.begin(), ::tolower);

        int nodes = parse_int(fields[2]);
        bool available = (state.find("idle") != std::string::npos ||
                          state.find("mix") != std::string::npos);

        gpu_log(fmt::format("  found: partition={} gpu_type={} gpu_count={} nodes={} state={} avail={}",
                            partition, gpu_type, gpu_count, nodes, state, available));

        bool merged = false;
        for (auto& r : resources) {
            if (r.partition == partition && r.gpu_type == gpu_type) {
                r.total_nodes += nodes;
                if (available) r.avail_nodes += nodes;
                merged = true;
                break;
            }
        }

        if (!merged) {
            GpuResource r;
            r.partition = partition;
            r.gpu_type = gpu_type;
            r.gpu_per_node = gpu_count;
            r.total_nodes = nodes;
            r.avail_nodes = available ? nodes : 0;
            r.mem_mb = parse_int(fields[3]);
            r.cpus_per_node = parse_int(fields[4]);
            resources.push_back(r);
        }
    }

    gpu_log(fmt::format("discover_gpu_resources: found {} entries", resources.size()));
    return resources;
}

// ── Query: user partitions ───────────────────────────────────

std::vector<std::string> discover_user_partitions(SSHConnection& login,
                                                   const std::string& username) {
    std::set<std::string> partitions;
    bool has_wildcard = false;

    // Primary: sacctmgr associations
    auto result = login.run(
        "sacctmgr show assoc where user=" + username +
        " format=partition -n -p 2>/dev/null");

    gpu_log(fmt::format("sacctmgr exit={} stdout_len={}", result.exit_code,
                        result.stdout_data.size()));
    if (!result.stdout_data.empty()) {
        gpu_log("sacctmgr output: " + result.stdout_data.substr(0, 500));
    }

    if (result.exit_code == 0 && !result.stdout_data.empty()) {
        std::istringstream iss(result.stdout_data);
        std::string line;
        while (std::getline(iss, line)) {
            line = trim(line);
            // sacctmgr -p output has trailing '|'
            if (!line.empty() && line.back() == '|')
                line.pop_back();
            line = trim(line);
            if (line.empty()) {
                // Empty partition = default/wildcard association = all partitions
                has_wildcard = true;
                gpu_log("  sacctmgr: found wildcard (empty) partition — user has access to all");
            } else {
                partitions.insert(line);
                gpu_log("  sacctmgr: partition=" + line);
            }
        }
    }

    // If user has a wildcard association, they can access everything — return empty
    // (empty = no filter in find_gpu_partition)
    if (has_wildcard) {
        gpu_log("discover_user_partitions: wildcard access, returning empty (no filter)");
        return {};
    }

    // Fallback: if sacctmgr returned nothing useful, use sinfo
    if (partitions.empty()) {
        gpu_log("sacctmgr returned nothing, falling back to sinfo");
        result = login.run("sinfo -h -o '%P' 2>/dev/null");
        if (result.exit_code == 0) {
            std::istringstream iss(result.stdout_data);
            std::string line;
            while (std::getline(iss, line)) {
                line = trim(line);
                if (!line.empty() && line.back() == '*')
                    line.pop_back();
                if (!line.empty()) {
                    partitions.insert(line);
                }
            }
        }
        gpu_log(fmt::format("sinfo fallback: found {} partitions", partitions.size()));
    }

    return std::vector<std::string>(partitions.begin(), partitions.end());
}

// ── Selection: find best partition ───────────────────────────

PartitionMatch find_gpu_partition(const std::vector<GpuResource>& resources,
                                   const std::string& gpu_type,
                                   int gpu_count,
                                   const std::vector<std::string>& user_partitions) {
    PartitionMatch result;

    if (resources.empty()) {
        result.error = "No GPU resources found on this cluster";
        return result;
    }

    std::set<std::string> allowed(user_partitions.begin(), user_partitions.end());
    bool filter_by_user = !allowed.empty();

    gpu_log(fmt::format("find_gpu_partition: looking for gpu_type='{}' count={} filter_by_user={} ({})",
                        gpu_type, gpu_count, filter_by_user, user_partitions.size()));

    struct Candidate {
        const GpuResource* resource;
        int score;
    };
    std::vector<Candidate> candidates;

    for (const auto& r : resources) {
        // Filter: user permission
        if (filter_by_user && allowed.find(r.partition) == allowed.end()) {
            gpu_log(fmt::format("  skip {} (not in user partitions)", r.partition));
            continue;
        }

        // Filter: GPU type must match (substring/case-insensitive)
        if (!type_matches(r.gpu_type, gpu_type)) {
            gpu_log(fmt::format("  skip {} (type '{}' doesn't match '{}')",
                                r.partition, r.gpu_type, gpu_type));
            continue;
        }

        // Filter: must have enough GPUs per node
        if (r.gpu_per_node < gpu_count) {
            gpu_log(fmt::format("  skip {} (only {} gpus, need {})",
                                r.partition, r.gpu_per_node, gpu_count));
            continue;
        }

        int score = 0;
        if (r.avail_nodes > 0) score += 1000;
        score -= (r.gpu_per_node - gpu_count) * 10;
        score += r.total_nodes;

        gpu_log(fmt::format("  candidate: {} type={} gpus={} score={}",
                            r.partition, r.gpu_type, r.gpu_per_node, score));
        candidates.push_back({&r, score});
    }

    if (candidates.empty()) {
        // Build helpful error with what we DID find
        std::string available_types;
        for (const auto& r : resources) {
            if (!available_types.empty()) available_types += ", ";
            available_types += r.partition + ":" + r.gpu_type + ":" + std::to_string(r.gpu_per_node);
        }

        if (filter_by_user) {
            result.error = fmt::format("No accessible partition has gpu:{}:{}. "
                                       "Available GPU resources: [{}]",
                                       gpu_type, gpu_count, available_types);
        } else {
            result.error = fmt::format("No partition has gpu:{}:{}. "
                                       "Available GPU resources: [{}]",
                                       gpu_type, gpu_count, available_types);
        }
        return result;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.score > b.score;
              });

    auto& best = *candidates[0].resource;
    result.found = true;
    result.partition = best.partition;
    result.gpu_type = best.gpu_type;
    result.gpu_per_node = best.gpu_per_node;

    gpu_log(fmt::format("find_gpu_partition: selected partition={} type={} gpus={}",
                        result.partition, result.gpu_type, result.gpu_per_node));
    return result;
}

// ── Convenience: full resolve ────────────────────────────────

PartitionMatch resolve_gpu_partition(SSHConnection& login,
                                      const std::string& username,
                                      const std::string& current_partition,
                                      const std::string& gpu_type,
                                      int gpu_count) {
    if (gpu_count <= 0 && gpu_type.empty()) {
        PartitionMatch m;
        m.found = true;
        m.partition = current_partition;
        return m;
    }

    // If user explicitly set a non-default partition, trust it
    if (!current_partition.empty() && current_partition != "batch") {
        PartitionMatch m;
        m.found = true;
        m.partition = current_partition;
        m.gpu_type = gpu_type;
        return m;
    }

    auto resources = discover_gpu_resources(login);
    auto user_parts = discover_user_partitions(login, username);

    return find_gpu_partition(resources, gpu_type, gpu_count > 0 ? gpu_count : 1,
                              user_parts);
}
