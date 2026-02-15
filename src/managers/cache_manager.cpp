#include "cache_manager.hpp"
#include <core/constants.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <sstream>

CacheManager::CacheManager(SSHConnection& dtn, const std::string& username)
    : dtn_(dtn), username_(username),
      tccp_home_(fmt::format(REMOTE_TCCP_HOME, username)) {}

void CacheManager::ensure_within_cap(const std::string& current_project,
                                      const std::string& current_sif,
                                      StatusCallback cb) {
    // 1. Clean stale tccp temp files:
    //    - Old home-dir singularity cache/tmp (legacy, no longer used for pulls)
    //    - DTN pip temp (interrupted venv creation)
    dtn_.run(fmt::format(
        "rm -rf {0}/container-cache/cache/* {0}/container-cache/tmp/* "
        "/tmp/{1}/tccp-pip-tmp 2>/dev/null; true",
        tccp_home_, username_));

    // 2. Discovery: get total usage, container sizes+mtimes, venv sizes+mtimes
    //    All in a single SSH round-trip.
    std::string discovery_cmd = fmt::format(
        "echo '===TOTAL==='; "
        "du -sb {0} 2>/dev/null | cut -f1; "
        "echo '===CONTAINERS==='; "
        "for f in {0}/container-cache/images/*.sif; do "
        "  [ -f \"$f\" ] && stat -c '%s %Y %n' \"$f\" 2>/dev/null; "
        "done; "
        "echo '===VENVS==='; "
        "for d in {0}/projects/*/env; do "
        "  [ -d \"$d\" ] && {{"
        "    sz=$(du -sb \"$d\" 2>/dev/null | cut -f1); "
        "    if [ -f \"$d/.last_used\" ]; then "
        "      mt=$(stat -c '%Y' \"$d/.last_used\" 2>/dev/null); "
        "    else "
        "      mt=0; "
        "    fi; "
        "    echo \"$sz $mt $d\"; "
        "  }}; "
        "done; "
        "echo '===END==='",
        tccp_home_);

    auto result = dtn_.run(discovery_cmd);
    if (result.exit_code != 0) {
        return; // can't discover, skip eviction
    }

    // 3. Parse discovery output
    int64_t total_bytes = 0;
    std::vector<CacheItem> containers;
    std::vector<CacheItem> venvs;

    std::istringstream stream(result.stdout_data);
    std::string line;
    enum Section { NONE, TOTAL, CONTAINERS, VENVS } section = NONE;

    while (std::getline(stream, line)) {
        if (line.find("===TOTAL===") != std::string::npos) { section = TOTAL; continue; }
        if (line.find("===CONTAINERS===") != std::string::npos) { section = CONTAINERS; continue; }
        if (line.find("===VENVS===") != std::string::npos) { section = VENVS; continue; }
        if (line.find("===END===") != std::string::npos) break;
        if (line.empty()) continue;

        if (section == TOTAL) {
            try { total_bytes = std::stoll(line); } catch (...) {}
        } else if (section == CONTAINERS) {
            // format: size mtime path
            std::istringstream ls(line);
            CacheItem item;
            item.type = CacheItem::CONTAINER;
            ls >> item.size_bytes >> item.mtime_epoch;
            std::getline(ls >> std::ws, item.path);
            if (!item.path.empty()) containers.push_back(item);
        } else if (section == VENVS) {
            // format: size mtime path
            std::istringstream ls(line);
            CacheItem item;
            item.type = CacheItem::VENV;
            ls >> item.size_bytes >> item.mtime_epoch;
            std::getline(ls >> std::ws, item.path);
            if (!item.path.empty()) venvs.push_back(item);
        }
    }

    // 4. Check if under cap
    if (total_bytes <= CACHE_SOFT_CAP_BYTES) {
        return;
    }

    if (cb) cb(fmt::format("Storage usage {:.1f}GB exceeds {:.0f}GB cap, checking for evictable items...",
                           total_bytes / (1024.0 * 1024.0 * 1024.0),
                           CACHE_SOFT_CAP_BYTES / (1024.0 * 1024.0 * 1024.0)));

    // 5. Build eviction candidates (exclude current project's venv and current container)
    std::vector<CacheItem> candidates;

    // Sort venvs by mtime ascending (oldest first) — cheaper to recreate
    std::sort(venvs.begin(), venvs.end(),
              [](const CacheItem& a, const CacheItem& b) { return a.mtime_epoch < b.mtime_epoch; });
    for (auto& v : venvs) {
        // Exclude current project's env dir
        std::string current_env = fmt::format("{}/projects/{}/env", tccp_home_, current_project);
        if (v.path == current_env) continue;
        candidates.push_back(v);
    }

    // Sort containers by mtime ascending (oldest first) — expensive to recreate
    std::sort(containers.begin(), containers.end(),
              [](const CacheItem& a, const CacheItem& b) { return a.mtime_epoch < b.mtime_epoch; });
    for (auto& c : containers) {
        // Exclude current SIF
        if (c.path.find(current_sif) != std::string::npos) continue;
        candidates.push_back(c);
    }

    // 6. Walk candidates, accumulate deletions until projected <= cap
    std::vector<std::string> to_delete;
    int64_t projected = total_bytes;
    for (const auto& item : candidates) {
        if (projected <= CACHE_SOFT_CAP_BYTES) break;
        to_delete.push_back(item.path);
        projected -= item.size_bytes;

        std::string type_str = (item.type == CacheItem::VENV) ? "venv" : "container";
        if (cb) cb(fmt::format("  Evicting {} ({:.1f}GB): {}",
                               type_str, item.size_bytes / (1024.0 * 1024.0 * 1024.0), item.path));
    }

    if (to_delete.empty()) {
        if (cb) cb("No evictable items found — proceeding anyway");
        return;
    }

    // 7. Batch delete in a single SSH command
    std::string rm_cmd = "rm -rf";
    for (const auto& path : to_delete) {
        rm_cmd += " " + path;
    }
    dtn_.run(rm_cmd);

    if (cb) cb(fmt::format("Evicted {} item(s), projected usage: {:.1f}GB",
                           to_delete.size(), projected / (1024.0 * 1024.0 * 1024.0)));
}

void CacheManager::touch_used(const std::string& sif_path,
                               const std::string& project_env_path) {
    dtn_.run(fmt::format("touch {} {}/.last_used", sif_path, project_env_path));
}
