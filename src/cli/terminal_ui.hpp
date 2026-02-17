#pragma once

#include <string>

// Terminal UI primitives: status bar, alternate screen.
namespace TerminalUI {

constexpr int HEADER_ROWS = 2;

// ── Terminal dimensions ──────────────────────────────────────────

int term_width();
int term_rows();

// ── Alternate screen management ──────────────────────────────────

void enter_alt_screen();
void leave_alt_screen();
void setup_content_area();  // Sets scroll region above status bar

// ── Status bar ───────────────────────────────────────────────────

struct StatusBar {
    std::string job_name;
    std::string slurm_id;
    std::string node;
    std::string status;        // e.g. "RUNNING", "INITIALIZING..."
    std::string controls;      // e.g. "Ctrl+C cancel  Ctrl+\\ detach"
    std::string l2_bg;         // ANSI bg color for line 2 (empty = default grey)
    std::string l2_fg;         // ANSI fg color for line 2 (empty = default)
};
void draw_status_bar(const StatusBar& bar);

} // namespace TerminalUI
