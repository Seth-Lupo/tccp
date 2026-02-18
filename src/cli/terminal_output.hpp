#pragma once

#include <string>

// Terminal output processing: ANSI stripping and noise suppression.
//
// All incoming SSH bytes pass through strip_ansi() to produce plain text.
namespace TerminalOutput {

// Strip all ANSI escape sequences (CSI, OSC, ESC+char), returning plain text.
// Preserves \r, \n, \t, and printable characters (>= 0x20).
std::string strip_ansi(const char* buf, int len);

// Remove entire lines containing known noise patterns (dtach, script markers).
void suppress_noise(std::string& text);

} // namespace TerminalOutput
