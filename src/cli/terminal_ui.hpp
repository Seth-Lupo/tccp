#pragma once

#include <string>
#include <vector>
#include <functional>

// Terminal UI primitives: viewport rendering, scrollback, exit prompt
//
// This module provides the building blocks for the job viewer's terminal interface,
// handling scroll calculations, viewport redraws, and the post-job exit UI.
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

// ── Scroll calculations ─────────────────────────────────────────

// Get height of scrollable content area (terminal height - header)
int get_scroll_height();

// Compute max scroll offset: allow scrolling until just 1 line remains visible
int compute_max_scroll(int total_lines);

// Compute scroll offset to show top of buffer (fill viewport)
int compute_top_scroll(int total_lines);

// ── Viewport rendering ──────────────────────────────────────────

// Redraw visible portion of output based on scroll offset
void redraw_viewport(const std::vector<std::string>& output_lines, int scroll_offset);

// ── Exit scrollback UI ──────────────────────────────────────────

// Show exit prompt and allow scrolling through output buffer. Returns on Enter.
void wait_for_exit_with_scroll(
    std::vector<std::string>& output_lines,
    int exit_status,
    std::function<void()> draw_header_fn);

} // namespace TerminalUI
