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

// ── Variant helpers ─────────────────────────────────────────

const GpuVariant* find_variant_by_id(const std::string& id) {
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& v : gpu_variants()) {
        std::string vid = v.id;
        std::transform(vid.begin(), vid.end(), vid.begin(), ::tolower);
        if (vid == lower) return &v;
    }
    return nullptr;
}

std::vector<const GpuVariant*> find_variants_by_base(const std::string& base_type) {
    std::string lower = base_type;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::vector<const GpuVariant*> result;
    for (const auto& v : gpu_variants()) {
        std::string vbase = v.base_type;
        std::transform(vbase.begin(), vbase.end(), vbase.begin(), ::tolower);
        if (vbase == lower) result.push_back(&v);
    }
    return result;
}

// Extract the GRES base type from a variant ID.  "a100-40gb" → "a100".
// If id is not a known variant, returns it unchanged.
static std::string base_type_for(const std::string& gpu_type) {
    const auto* v = find_variant_by_id(gpu_type);
    return v ? v->base_type : gpu_type;
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

    // Exact match (handles variant IDs like "a100-40gb" == "a100-40gb")
    if (r == q) return true;

    // If the request is a bare base type (e.g. "a100"), match any variant
    // whose base_type matches (e.g. resource "a100-40gb" has base "a100").
    std::string r_base = base_type_for(r);
    std::string q_base = base_type_for(q);
    // If user asked for a specific variant (q != q_base), only exact match works
    if (q != q_base) return false;
    // Bare base type: match if resource's base type matches
    if (r_base == q) return true;

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

// Check if any node in a comma/space-separated node list starts with a prefix
static bool nodes_have_prefix(const std::string& nodelist, const std::string& prefix) {
    // nodelist can be "cc1gpu001,cc1gpu002" or "cc1gpu[001-003]" or a single name
    std::string lower_list = nodelist;
    std::transform(lower_list.begin(), lower_list.end(), lower_list.begin(), ::tolower);
    std::string lower_prefix = prefix;
    std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);
    return lower_list.find(lower_prefix) != std::string::npos;
}

// Split a single sinfo row into variant-specific GpuResource entries.
// If no variant matches, returns one entry with the raw gpu_type.
static std::vector<GpuResource> split_by_variant(
        const std::string& partition, const std::string& raw_gpu_type,
        int gpu_count, int nodes, bool available,
        int mem_mb, int cpus_per_node, const std::string& nodelist) {

    std::vector<GpuResource> out;
    auto variants = find_variants_by_base(raw_gpu_type);

    if (variants.empty() || nodelist.empty()) {
        // No variants for this type — keep raw
        GpuResource r;
        r.partition = partition;
        r.gpu_type = raw_gpu_type;
        r.gpu_per_node = gpu_count;
        r.total_nodes = nodes;
        r.avail_nodes = available ? nodes : 0;
        r.mem_mb = mem_mb;
        r.cpus_per_node = cpus_per_node;
        out.push_back(r);
        return out;
    }

    bool any_matched = false;
    for (const auto* v : variants) {
        if (nodes_have_prefix(nodelist, v->node_prefix)) {
            GpuResource r;
            r.partition = partition;
            r.gpu_type = v->id;  // "a100-40gb" instead of "a100"
            r.gpu_per_node = gpu_count;
            r.total_nodes = nodes;
            r.avail_nodes = available ? nodes : 0;
            r.mem_mb = mem_mb;
            r.cpus_per_node = cpus_per_node;
            r.node_prefix = v->node_prefix;
            out.push_back(r);
            any_matched = true;
        }
    }

    if (!any_matched) {
        // Node names didn't match any known variant — keep raw
        GpuResource r;
        r.partition = partition;
        r.gpu_type = raw_gpu_type;
        r.gpu_per_node = gpu_count;
        r.total_nodes = nodes;
        r.avail_nodes = available ? nodes : 0;
        r.mem_mb = mem_mb;
        r.cpus_per_node = cpus_per_node;
        out.push_back(r);
    }

    return out;
}

std::vector<GpuResource> discover_gpu_resources(SSHConnection& login) {
    // %N = nodelist — needed to distinguish GPU variants by hostname prefix
    auto result = login.run(
        "sinfo -h -o '%P|%G|%D|%m|%c|%T|%N' 2>/dev/null");

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

        std::string nodelist = (fields.size() >= 7) ? fields[6] : "";

        gpu_log(fmt::format("  found: partition={} gpu_type={} gpu_count={} nodes={} state={} avail={} nodelist={}",
                            partition, gpu_type, gpu_count, nodes, state, available, nodelist));

        auto split = split_by_variant(partition, gpu_type, gpu_count, nodes, available,
                                       parse_int(fields[3]), parse_int(fields[4]), nodelist);

        for (auto& entry : split) {
            bool merged = false;
            for (auto& r : resources) {
                if (r.partition == entry.partition && r.gpu_type == entry.gpu_type) {
                    r.total_nodes += entry.total_nodes;
                    r.avail_nodes += entry.avail_nodes;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                resources.push_back(std::move(entry));
            }
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

        // Tier penalty: prefer cheapest GPU variant (lower tier = cheaper)
        const auto* variant = find_variant_by_id(r.gpu_type);
        if (variant) {
            score -= variant->tier * 5;
        }

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
    result.node_prefix = best.node_prefix;

    gpu_log(fmt::format("find_gpu_partition: selected partition={} type={} gpus={} node_prefix={}",
                        result.partition, result.gpu_type, result.gpu_per_node, result.node_prefix));
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
