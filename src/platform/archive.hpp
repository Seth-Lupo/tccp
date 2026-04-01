#pragma once

#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <cstdint>

namespace platform {

// Create a tar archive at tar_path containing the specified files
// from base_dir. Each entry in files is a relative path from base_dir.
void create_tar(const std::filesystem::path& tar_path,
                const std::filesystem::path& base_dir,
                const std::vector<std::string>& files);

// Stream a tar archive through a callback. write_fn is called with each
// chunk of tar data; it must return the number of bytes consumed (or < 0
// on error). No temp file is created.
using TarWriteFn = std::function<int64_t(const void*, size_t)>;
void create_tar_to_callback(const std::filesystem::path& base_dir,
                             const std::vector<std::string>& files,
                             TarWriteFn write_fn);

} // namespace platform
