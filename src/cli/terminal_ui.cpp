#include "terminal_ui.hpp"
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <fmt/format.h>
#include <algorithm>

namespace TerminalUI {

// ── Terminal dimensions ──────────────────────────────────────────

int term_width() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

int term_rows() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
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

int get_scroll_height() {
    return term_rows() - HEADER_ROWS;
}

int compute_max_scroll(int total_lines) {
    return std::max(0, total_lines - 1);
}

int compute_top_scroll(int total_lines) {
    int height = get_scroll_height();
    // Scroll to show line 0 at the top of viewport
    return total_lines - 1;
}

void redraw_viewport(const std::vector<std::string>& output_lines, int scroll_offset) {
    int height = get_scroll_height();
    int total_lines = output_lines.size();

    // Clear scroll area and move to top
    write(STDOUT_FILENO, "\033[H\033[J", 6);

    // Calculate which lines to show
    int end_line = total_lines - scroll_offset;
    int start_line = std::max(0, end_line - height);

    // Draw visible lines
    for (int i = start_line; i < end_line && i < total_lines; i++) {
        write(STDOUT_FILENO, output_lines[i].c_str(), output_lines[i].size());
    }
}

void wait_for_exit_with_scroll(
    std::vector<std::string>& output_lines,
    int exit_status,
    std::function<void()> draw_header_fn)
{
    int scroll_offset = 0;

    // Draw initial view at bottom
    redraw_viewport(output_lines, scroll_offset);

    // Position exit prompt within scroll region (above header)
    struct winsize ws;
    int rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        rows = ws.ws_row;
    int prompt_row = std::max(1, rows - HEADER_ROWS);

    auto draw_exit_prompt = [&]() {
        std::string exit_msg;
        if (exit_status == 0)
            exit_msg = fmt::format(
                "\033[{};1H\033[2m[exit 0]\033[0m  \033[2mPress Enter to leave\033[0m",
                prompt_row);
        else
            exit_msg = fmt::format(
                "\033[{};1H\033[91m[exit {}]\033[0m  \033[2mPress Enter to leave\033[0m",
                prompt_row, exit_status);
        write(STDOUT_FILENO, exit_msg.data(), exit_msg.size());
        draw_header_fn();
    };

    draw_exit_prompt();

    // Scrollback control loop
    for (;;) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;

        // Ctrl+\ → exit
        if (c == 0x1c) break;

        // Ctrl+T → scroll to top (show full screen)
        if (c == 0x14) {
            scroll_offset = compute_top_scroll(output_lines.size());
            redraw_viewport(output_lines, scroll_offset);
            draw_exit_prompt();
            continue;
        }

        if (c == 0x1b) {
            char seq[2];
            if (read(STDIN_FILENO, seq, 2) == 2 && seq[0] == '[') {
                if (seq[1] == 'A') {  // Up arrow
                    int max_scroll = compute_max_scroll(output_lines.size());
                    if (scroll_offset < max_scroll) {
                        scroll_offset++;
                        redraw_viewport(output_lines, scroll_offset);
                        draw_exit_prompt();
                    }
                } else if (seq[1] == 'B') {  // Down arrow
                    if (scroll_offset > 0) {
                        scroll_offset--;
                        redraw_viewport(output_lines, scroll_offset);
                        draw_exit_prompt();
                    }
                }
            }
            continue;
        }

        if (c == '\r' || c == '\n') break;
    }
}

} // namespace TerminalUI
