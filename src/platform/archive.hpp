#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace platform {

// Create a tar archive at tar_path containing the specified files
// from base_dir. Each entry in files is a relative path from base_dir.
void create_tar(const std::filesystem::path& tar_path,
                const std::filesystem::path& base_dir,
                const std::vector<std::string>& files);

} // namespace platform
