#include "marker_protocol.hpp"
#include <core/utils.hpp>

std::string build_marker_command(const std::string& cmd) {
    // Shell string concatenation (BEG''IN / DO''NE) prevents the literal marker
    // from appearing in PTY echo. The echo commands still output the full strings.
    if (cmd.find('\n') == std::string::npos) {
        // Single-line: keep DONE echo on the same line with ';'.
        // Prevents SSH hop commands from consuming the DONE marker.
        return "echo __TCCP_BEG''IN__; " + cmd +
               "; echo __TCCP_DO''NE__ $?\n";
    } else {
        // Multi-line (heredocs): DONE on separate line so heredoc terminators aren't mangled.
        return "echo __TCCP_BEG''IN__; " + cmd +
               "\necho __TCCP_DO''NE__ $?\n";
    }
}

MarkerResult parse_marker_output(const std::string& raw) {
    std::string done_marker = "__TCCP_DONE__";
    std::string begin_marker = "__TCCP_BEGIN__";

    auto done_pos = raw.find(done_marker);
    if (done_pos == std::string::npos) {
        return {"", 0, false};
    }

    // Parse exit code after DONE marker
    int exit_code = 0;
    auto code_start = done_pos + done_marker.length();
    if (code_start < raw.length()) {
        std::string code_str = raw.substr(code_start);
        auto num_start = code_str.find_first_of("0123456789");
        if (num_start != std::string::npos) {
            exit_code = safe_stoi(code_str.substr(num_start), 0);
        }
    }

    // Extract text between BEGIN and DONE markers
    std::string clean;
    auto begin_pos = raw.find(begin_marker);
    if (begin_pos != std::string::npos) {
        auto content_start = begin_pos + begin_marker.length();
        if (content_start < raw.length() && raw[content_start] == '\r')
            content_start++;
        if (content_start < raw.length() && raw[content_start] == '\n')
            content_start++;
        clean = raw.substr(content_start, done_pos - content_start);
    } else {
        clean = raw.substr(0, done_pos);
    }

    // Strip trailing whitespace
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r'
                           || clean.back() == ' ' || clean.back() == '\t'))
        clean.pop_back();

    // Strip trailing prompt echo of the sentinel command
    auto last_nl = clean.rfind('\n');
    if (last_nl != std::string::npos) {
        auto trailing = clean.substr(last_nl + 1);
        if (trailing.find("__TCCP_DO") != std::string::npos) {
            clean = clean.substr(0, last_nl);
        }
    } else if (clean.find("__TCCP_DO") != std::string::npos) {
        clean.clear();
    }

    // Trim remaining trailing whitespace
    auto last_content = clean.find_last_not_of(" \t\r\n");
    if (last_content != std::string::npos) {
        clean = clean.substr(0, last_content + 1);
    } else {
        clean.clear();
    }

    return {clean, exit_code, true};
}
