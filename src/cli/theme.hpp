#pragma once

#include <string>
#include <fmt/format.h>

namespace theme {

// Tufts colors (ANSI escape sequences)
// Tufts Blue:  #3E78B2
// Tufts Brown: #80633A
namespace color {
    const std::string BLUE      = "\033[38;2;62;120;178m";
    const std::string BROWN     = "\033[38;2;128;99;58m";
    const std::string WHITE     = "\033[97m";
    const std::string GRAY      = "\033[90m";
    const std::string RED       = "\033[91m";
    const std::string GREEN     = "\033[92m";
    const std::string YELLOW    = "\033[93m";
    const std::string BOLD      = "\033[1m";
    const std::string DIM       = "\033[2m";
    const std::string RESET     = "\033[0m";
}

// Shorthand wrappers
inline std::string blue(const std::string& s)   { return color::BLUE + s + color::RESET; }
inline std::string brown(const std::string& s)   { return color::BROWN + s + color::RESET; }
inline std::string bold(const std::string& s)    { return color::BOLD + s + color::RESET; }
inline std::string dim(const std::string& s)     { return color::DIM + s + color::RESET; }
inline std::string green(const std::string& s)   { return color::GREEN + s + color::RESET; }
inline std::string red(const std::string& s)     { return color::RED + s + color::RESET; }
inline std::string yellow(const std::string& s)  { return color::YELLOW + s + color::RESET; }
inline std::string white(const std::string& s)   { return color::WHITE + s + color::RESET; }

// ── Layout ──────────────────────────────────────────────

// Just the horizontal line (no extra spacing — callers control gaps)
inline std::string rule() {
    return color::DIM + "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" + color::RESET + "\n";
}

// Banner — clears screen, then title + rule
inline std::string banner() {
    return
        "\033[2J\033[H\n"
        + color::BLUE + color::BOLD
        + "  Tufts Cluster Command Prompt\n"
        + color::RESET + color::DIM + "  v0.3.0\n"
        + "  Powered by Jumbo"
        + color::RESET + "\n\n"
        + rule();
}

// Section header — blank line before title, blank line after
inline std::string section(const std::string& title) {
    return "\n" + color::BROWN + color::BOLD + "  " + title + color::RESET + "\n\n";
}

// Legacy alias
inline std::string header(const std::string& title) { return section(title); }

// Divider — blank line, rule, blank line (separates content blocks)
inline std::string divider() {
    return "\n" + rule() + "\n";
}

// ── Status indicators ───────────────────────────────────

inline std::string ok(const std::string& msg) {
    return color::GREEN + "    + " + color::RESET + msg + "\n";
}

inline std::string fail(const std::string& msg) {
    return color::RED + "    x " + color::RESET + msg + "\n";
}

inline std::string info(const std::string& msg) {
    return color::BLUE + "    ~ " + color::RESET + msg + "\n";
}

inline std::string step(const std::string& msg) {
    return color::BROWN + "    > " + color::RESET + msg + "\n";
}

// Subtle log line for internal status (dimmer than program output)
inline std::string log(const std::string& msg) {
    return "\033[38;2;80;80;80m    \xc2\xb7 " + msg + "\033[0m\n";
}

// Key-value row for dashboard/status panels
inline std::string kv(const std::string& key, const std::string& value) {
    return color::DIM + fmt::format("    {:<10}", key) + color::RESET + value + "\n";
}

} // namespace theme
