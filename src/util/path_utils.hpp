#pragma once

#include <filesystem>

namespace PathUtils {
bool is_gitignored(const std::string& path);
}
