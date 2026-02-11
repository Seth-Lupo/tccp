#pragma once

#include <string>
#include <vector>
#include <cstdio>

// Terminal output processing: filtering, echo suppression, scrollback management
//
// This module handles all output from remote SSH sessions, ensuring clean display
// by filtering escape sequences, suppressing echoed input, and detecting job markers.
namespace TerminalOutput {

// Strip SGR colors, keep text. For capture files.
std::string filter_for_capture(const char* buf, int len);

// Strip destructive sequences (clear, home, alt screen). For display.
std::string filter_for_display(const char* buf, int n, int& consecutive_newlines);

// Process channel output: filter, detect job start marker, update scrollback, write to capture
void process_channel_output(
    const char* buf, int n,
    std::vector<std::string>& output_lines,
    bool& job_started,
    bool should_clear_on_start,
    int& consecutive_newlines,
    FILE* capture,
    bool write_to_screen,
    std::string* pending_suppress = nullptr);

} // namespace TerminalOutput
