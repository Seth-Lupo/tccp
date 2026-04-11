#pragma once

#include <string>

#ifndef TCCP_VERSION
#define TCCP_VERSION "1.0.0"
#endif

namespace theme {

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

inline std::string blue(const std::string& s)   { return color::BLUE + s + color::RESET; }
inline std::string brown(const std::string& s)   { return color::BROWN + s + color::RESET; }
inline std::string bold(const std::string& s)    { return color::BOLD + s + color::RESET; }
inline std::string dim(const std::string& s)     { return color::DIM + s + color::RESET; }
inline std::string green(const std::string& s)   { return color::GREEN + s + color::RESET; }
inline std::string red(const std::string& s)     { return color::RED + s + color::RESET; }
inline std::string yellow(const std::string& s)  { return color::YELLOW + s + color::RESET; }
inline std::string white(const std::string& s)   { return color::WHITE + s + color::RESET; }

// ── Layout ──────────────────────────────────────────────

inline std::string rule() {
    return color::DIM + "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" + color::RESET + "\n";
}

inline std::string banner() {
    return
        "\n"
        + color::BLUE + color::BOLD
        + "  tccp\n"
        + color::RESET + color::DIM + "  v" + TCCP_VERSION + "\n"
        + color::RESET + "\n"
        + rule();
}

inline std::string section(const std::string& title) {
    return "\n" + color::BROWN + color::BOLD + "  " + title + color::RESET + "\n\n";
}

inline std::string divider() {
    return "\n" + rule() + "\n";
}

// ── Status indicators ───────────────────────────────────

inline std::string ok(const std::string& msg) {
    return color::DIM + msg + color::RESET + "\n";
}

inline std::string check(const std::string& msg) {
    return color::BLUE + "    ~ " + color::RESET + msg + "\n";
}

inline std::string error(const std::string& msg) {
    return color::RED + msg + color::RESET + "\n";
}

inline std::string info(const std::string& msg) {
    return color::BLUE + msg + color::RESET + "\n";
}

inline std::string step(const std::string& msg) {
    return color::DIM + "      " + msg + color::RESET + "\n";
}

inline std::string log(const std::string& msg) {
    return color::DIM + msg + color::RESET + "\n";
}

inline std::string kv(const std::string& key, const std::string& value) {
    return color::DIM + "    " + key + color::RESET + "  " + value + "\n";
}

} // namespace theme
