#include "terminal_output.hpp"

namespace TerminalOutput {

std::string strip_ansi(const char* buf, int len) {
    std::string out;
    out.reserve(len);

    for (int i = 0; i < len; ) {
        if (buf[i] == '\033') {
            if (i + 1 < len && buf[i + 1] == '[') {
                // CSI sequence: \033[ params final_byte
                i += 2;
                while (i < len && ((buf[i] >= '0' && buf[i] <= '9') ||
                                   buf[i] == ';' || buf[i] == '?'))
                    i++;
                if (i < len) i++;       // skip final byte
            } else if (i + 1 < len && buf[i + 1] == ']') {
                // OSC sequence: skip until BEL or ST
                i += 2;
                while (i < len && buf[i] != '\007' &&
                       !(buf[i] == '\033' && i + 1 < len && buf[i + 1] == '\\'))
                    i++;
                if (i < len && buf[i] == '\007')
                    i++;
                else if (i + 1 < len)
                    i += 2;             // ST = \033 backslash
            } else if (i + 1 < len) {
                i += 2;                 // ESC + single char
            } else {
                i++;                    // lone ESC at end
            }
        } else if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t') {
            out += buf[i++];            // keep line control + tabs
        } else if (static_cast<unsigned char>(buf[i]) >= 32) {
            out += buf[i++];            // printable
        } else {
            i++;                        // drop other control chars
        }
    }

    return out;
}

// ── Internal helper ─────────────────────────────────────────────

static void suppress_line(std::string& s, const char* pat) {
    std::string::size_type pos;
    while ((pos = s.find(pat)) != std::string::npos) {
        auto start = s.rfind('\n', pos);
        start = (start == std::string::npos) ? 0 : start;
        auto end = s.find('\n', pos);
        end = (end == std::string::npos) ? s.size() : end + 1;
        s.erase(start, end - start);
    }
}

void suppress_noise(std::string& text) {
    suppress_line(text, "EOF - dtach");
    suppress_line(text, "[detached]");
    suppress_line(text, "Connection to");
    suppress_line(text, "Script started on");
    suppress_line(text, "Script done on");
}

} // namespace TerminalOutput
