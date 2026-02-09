#pragma once

#include <string>
#include <vector>

namespace StringUtils {
std::vector<std::string> split(const std::string& str, char delimiter);
std::string trim(const std::string& str);
}
