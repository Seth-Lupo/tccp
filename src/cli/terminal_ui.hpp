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
