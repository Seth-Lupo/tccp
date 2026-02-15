#pragma once

// ── SSH options ─────────────────────────────────────────────
// Standard options for SSH hops through DTN to compute nodes.
constexpr const char* SSH_OPTS = "-o StrictHostKeyChecking=no -o BatchMode=yes";
constexpr const char* SSH_OPTS_FAST = "-o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=3";

// ── Timeouts ────────────────────────────────────────────────
constexpr int SSH_CMD_TIMEOUT_SECS       = 300;   // Max time for a single SSH command
constexpr int CONTAINER_PULL_TIMEOUT_SECS = 1800;  // 30 min for container pull
constexpr int ALLOC_WAIT_POLL_SECS       = 5;     // Seconds between allocation state polls
constexpr int ALLOC_WAIT_MAX_ITERS       = 120;   // Max iterations (120 * 5s = 10 min)
constexpr int ALLOC_WAIT_REPORT_INTERVAL = 2;     // Report progress every N iterations
constexpr int SSH_RETRY_DELAY_SECS       = 5;     // Delay between SSH retries on transient failure
constexpr int POLL_INTERVAL_SECS         = 30;    // Auto-poll interval in REPL
constexpr int INIT_LOG_POLL_MS           = 500;   // Init log tailing interval
constexpr int DETACH_DOUBLE_ESC_MS       = 500;   // Double-ESC detection window

// ── Retry counts ────────────────────────────────────────────
constexpr int SSH_MAX_RETRIES            = 3;     // Retries for transient SSH failures
constexpr int SSH_CMD_MAX_RETRIES        = 2;     // Retries for individual SSH commands

// ── Buffer sizes ────────────────────────────────────────────
constexpr int SSH_READ_BUF_SIZE          = 4096;
constexpr int SSH_DRAIN_BUF_SIZE         = 4096;
constexpr int JOBVIEW_READ_BUF_SIZE      = 16384;

// ── Default resource values ─────────────────────────────────
constexpr int DEFAULT_ALLOC_MINUTES      = 240;   // 4 hours
constexpr const char* DEFAULT_MEMORY     = "4G";
constexpr const char* DEFAULT_PARTITION  = "batch";
constexpr const char* DEFAULT_ALLOC_TIME = "4:00:00";
constexpr const char* DEFAULT_JOB_TIME   = "0:05:00";

// ── Cache management ──────────────────────────────────────────
constexpr int64_t CACHE_SOFT_CAP_BYTES = 20LL * 1024 * 1024 * 1024;  // 20 GB

// ── Remote path templates ───────────────────────────────────
// Use fmt::format with these: fmt::format(REMOTE_PROJECT_BASE, username, project_name)
constexpr const char* REMOTE_PROJECT_BASE    = "/cluster/home/{}/tccp/projects/{}";
constexpr const char* REMOTE_TCCP_HOME       = "/cluster/home/{}/tccp";
constexpr const char* REMOTE_CONTAINER_CACHE = "/cluster/home/{}/tccp/container-cache";
constexpr const char* REMOTE_SCRATCH_DIR     = "/tmp/{}/{}/{}";
