#pragma once

#include <string>

// Shared marker protocol for BEGIN/DONE command wrapping.
// Used by SSHConnection::run(), ShellChannel::run(), and SessionMultiplexer::run().

struct MarkerResult {
    std::string output;
    int exit_code;
    bool found;
};

// Build a command string wrapped with BEGIN/DONE markers.
// Single-line commands keep DONE on the same line (prevents SSH hop stdin consumption).
// Multi-line commands (heredocs) put DONE on a separate line.
std::string build_marker_command(const std::string& cmd);

// Parse raw output for BEGIN/DONE markers.
// Extracts text between markers, strips echo artifacts, parses exit code.
MarkerResult parse_marker_output(const std::string& raw);

// Marker strings (for direct use in relay/feed code)
inline constexpr const char* TCCP_BEGIN_MARKER = "__TCCP_BEGIN__";
inline constexpr const char* TCCP_DONE_MARKER  = "__TCCP_DONE__";
