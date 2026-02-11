#include "terminal_output.hpp"
#include <cstring>
#include <unistd.h>

namespace TerminalOutput {

// ── Internal helpers ────────────────────────────────────────────

// Remove all occurrences of escape sequence `seq`
static void strip_seq(std::string& s, const char* seq) {
    int len = std::strlen(seq);
    std::string::size_type pos;
    while ((pos = s.find(seq)) != std::string::npos)
        s.erase(pos, len);
}

// Remove entire line containing pattern
static void suppress_line(std::string& s, const char* pat) {
    auto pos = s.find(pat);
    if (pos == std::string::npos) return;
    auto start = s.rfind('\n', pos);
    start = (start == std::string::npos) ? 0 : start;
    auto end = s.find('\n', pos);
    end = (end == std::string::npos) ? s.size() : end + 1;
    s.erase(start, end - start);
}

// ── Public API ──────────────────────────────────────────────────

std::string filter_for_capture(const char* buf, int len) {
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ) {
        if (buf[i] == '\033' && i + 1 < len && buf[i+1] == '[') {
            int j = i + 2;
            while (j < len && (buf[j] == ';' || (buf[j] >= '0' && buf[j] <= '9') || buf[j] == '?'))
                j++;
            if (j < len) {
                if (buf[j] == 'm')
                    out.append(buf + i, j + 1 - i);
                i = j + 1;
            } else {
                i = j;
            }
        } else if (buf[i] == '\033') {
            i += 2;
        } else if (buf[i] == '\r') {
            i++;
        } else {
            out += buf[i++];
        }
    }
    return out;
}

std::string filter_for_display(const char* buf, int n, int& consecutive_newlines) {
    std::string display;
    display.reserve(n);

    // Collapse runs of >2 consecutive blank lines
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (++consecutive_newlines <= 2) display += buf[i];
        } else if (buf[i] == '\r') {
            display += buf[i];
        } else {
            consecutive_newlines = 0;
            display += buf[i];
        }
    }

    // Strip destructive sequences
    strip_seq(display, "\033[?1049h");  // Alt screen enter
    strip_seq(display, "\033[?1049l");  // Alt screen exit
    strip_seq(display, "\033[?47h");
    strip_seq(display, "\033[?47l");
    strip_seq(display, "\033[2J");      // Clear screen
    strip_seq(display, "\033[H");       // Cursor home

    // Suppress noise lines
    suppress_line(display, "EOF - dtach");
    suppress_line(display, "[detached]");
    suppress_line(display, "Connection to");
    suppress_line(display, "Script started on");
    suppress_line(display, "Script done on");

    return display;
}

void process_channel_output(
    const char* buf, int n,
    std::vector<std::string>& output_lines,
    bool& job_started,
    bool should_clear_on_start,
    int& consecutive_newlines,
    FILE* capture,
    bool write_to_screen,
    std::string* pending_suppress)
{
    std::string display = filter_for_display(buf, n, consecutive_newlines);

    // Suppress remote echo of user input
    if (pending_suppress && !pending_suppress->empty() && !display.empty()) {
        if (display.find(*pending_suppress) == 0) {
            display = display.substr(pending_suppress->size());
            pending_suppress->clear();
        } else if (pending_suppress->find(display) == 0) {
            if (pending_suppress->size() >= display.size() &&
                pending_suppress->substr(0, display.size()) == display) {
                *pending_suppress = pending_suppress->substr(display.size());
                display.clear();
            }
        } else {
            pending_suppress->clear();
        }
    }

    if (!display.empty()) {
        // Detect job start marker
        if (!job_started && display.find("__TCCP_JOB_START__") != std::string::npos) {
            job_started = true;

            if (should_clear_on_start && write_to_screen) {
                write(STDOUT_FILENO, "\033[H\033[J", 6);
                output_lines.clear();
            }

            // Strip marker from display
            size_t marker_pos = display.find("__TCCP_JOB_START__");
            if (marker_pos != std::string::npos) {
                size_t start = marker_pos;
                size_t end = marker_pos + 18;
                if (start > 0 && display[start - 1] == '\n') start--;
                if (end < display.size() && display[end] == '\n') end++;
                display.erase(start, end - start);
            }
        }

        // Add to scrollback buffer (split by lines)
        size_t pos = 0;
        size_t newline_pos;
        while ((newline_pos = display.find('\n', pos)) != std::string::npos) {
            std::string line = display.substr(pos, newline_pos - pos + 1);
            output_lines.push_back(line);
            pos = newline_pos + 1;
        }
        // Add remaining partial line
        if (pos < display.size()) {
            if (!output_lines.empty()) {
                output_lines.back() += display.substr(pos);
            } else {
                output_lines.push_back(display.substr(pos));
            }
        }

        // Write to screen
        if (write_to_screen) {
            write(STDOUT_FILENO, display.data(), display.size());
        }

        // Write to capture file
        if (capture) {
            std::string filtered = filter_for_capture(display.data(), display.size());
            fwrite(filtered.data(), 1, filtered.size(), capture);
            fflush(capture);
        }
    }
}

} // namespace TerminalOutput
