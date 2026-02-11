#include "terminal_ui.hpp"
#include <sys/ioctl.h>
#include <unistd.h>
#include <fmt/format.h>
#include <algorithm>

namespace TerminalUI {

int get_scroll_height() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return ws.ws_row - HEADER_ROWS;
    }
    return 24 - HEADER_ROWS;
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
