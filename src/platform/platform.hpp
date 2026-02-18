#pragma once

#include <string>
#include <filesystem>

namespace platform {

// Returns the user's home directory (HOME on Unix, USERPROFILE on Windows).
std::filesystem::path home_dir();

// Returns the system temporary directory (/tmp on Unix, GetTempPath on Windows).
std::filesystem::path temp_dir();

// Creates a temporary file with the given prefix. Returns its path.
std::filesystem::path temp_file(const std::string& prefix);

// Sleep for the given number of milliseconds.
void sleep_ms(int ms);

} // namespace platform
