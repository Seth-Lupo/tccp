#include "terminal_ui.hpp"
#include <platform/terminal.hpp>
#include <iostream>
#include <fmt/format.h>
#include <algorithm>
#ifndef _WIN32
#  include <unistd.h>
#endif

namespace TerminalUI {

// ── Terminal dimensions ──────────────────────────────────────────

int term_width() {
    return platform::term_width();
}

int term_rows() {
    return platform::term_height();
}

// ── Alternate screen management ──────────────────────────────────

void enter_alt_screen() {
    std::cout << "\033[?1049h\033[H" << std::flush;
}

void leave_alt_screen() {
    std::cout << "\033[r\033[?1049l" << std::flush;
}

void setup_content_area() {
    int rows = term_rows();
    std::cout << fmt::format("\033[1;{}r", rows - 2);
    std::cout << "\033[1;1H" << std::flush;
}

// ── Status bar ───────────────────────────────────────────────────

void draw_status_bar(const StatusBar& bar) {
    int w = term_width();
    int h = term_rows();

    // Line 1: tccp | job_name | SLURM:id | node
    std::string l1 = " \033[38;2;62;120;178;1mtccp\033[22;38;2;178;178;178m";
    std::string l1_text = " tccp";
    if (!bar.job_name.empty())  { l1 += " | " + bar.job_name;         l1_text += " | " + bar.job_name; }
    if (!bar.slurm_id.empty())  { l1 += " | SLURM:" + bar.slurm_id;   l1_text += " | SLURM:" + bar.slurm_id; }
    if (!bar.node.empty())      { l1 += " | " + bar.node;              l1_text += " | " + bar.node; }
    int l1_pad = std::max(0, w - static_cast<int>(l1_text.size()));

    // Line 2: status + controls
    std::string l2_left = " " + bar.status;
    int l2_pad = std::max(0, w - static_cast<int>(l2_left.size()) - static_cast<int>(bar.controls.size()));

    std::string l2_bg = bar.l2_bg.empty() ? "\033[48;2;74;74;74m" : bar.l2_bg;
    std::string l2_fg = bar.l2_fg.empty() ? "\033[38;2;150;150;150m" : bar.l2_fg;

    std::string out;
    out.reserve(512);
    out += "\0337";  // Save cursor

    out += fmt::format("\033[{};1H", h - 1);
    out += "\033[48;2;62;62;62m\033[38;2;178;178;178m";
    out += l1;
    out.append(l1_pad, ' ');
    out += "\033[0m";

    out += fmt::format("\033[{};1H", h);
    out += l2_bg + l2_fg;
    out += l2_left;
    out.append(l2_pad, ' ');
    if (!bar.controls.empty()) {
        out += "\033[38;2;120;120;120m" + bar.controls;
    }
    out += "\033[K\033[0m";

    out += "\0338";  // Restore cursor
    write(STDOUT_FILENO, out.data(), out.size());
}

} // namespace TerminalUI
