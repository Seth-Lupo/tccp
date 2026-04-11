#include "archive.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace fs = std::filesystem;

namespace platform {

void create_tar(const fs::path& tar_path,
                const fs::path& base_dir,
                const std::vector<std::string>& files) {
    struct archive* a = archive_write_new();
    if (!a) throw std::runtime_error("Failed to create archive writer");

    if (archive_write_set_format_ustar(a) != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_write_free(a);
        throw std::runtime_error("Failed to set archive format: " + err);
    }

    if (archive_write_open_filename(a, tar_path.string().c_str()) != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_write_free(a);
        throw std::runtime_error("Failed to open tar file: " + err);
    }

    struct archive_entry* entry = archive_entry_new();

    for (const auto& rel_path : files) {
        fs::path full_path = base_dir / rel_path;

        if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) continue;

        auto file_size = fs::file_size(full_path);
        auto mtime = fs::last_write_time(full_path);
        auto mtime_sec = std::chrono::duration_cast<std::chrono::seconds>(
            mtime.time_since_epoch()).count();

        archive_entry_clear(entry);
        archive_entry_set_pathname(entry, rel_path.c_str());
        archive_entry_set_size(entry, static_cast<int64_t>(file_size));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, mtime_sec, 0);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            continue;  // skip files that fail
        }

        // Write file contents in chunks
        std::ifstream in(full_path, std::ios::binary);
        if (!in) continue;

        char buf[65536];
        while (in) {
            in.read(buf, sizeof(buf));
            auto bytes_read = in.gcount();
            if (bytes_read > 0) {
                if (archive_write_data(a, buf, static_cast<size_t>(bytes_read)) < 0) {
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    throw std::runtime_error("Failed to write data to archive");
                }
            }
        }
    }

    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);
}

void create_tar_to_callback(const fs::path& base_dir,
                             const std::vector<std::string>& files,
                             TarWriteFn write_fn) {
    struct archive* a = archive_write_new();
    if (!a) throw std::runtime_error("Failed to create archive writer");

    if (archive_write_set_format_ustar(a) != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_write_free(a);
        throw std::runtime_error("Failed to set archive format: " + err);
    }

    // Custom write callbacks for streaming
    struct CallbackData {
        TarWriteFn* fn;
    };
    CallbackData cb_data{&write_fn};

    auto open_cb = [](struct archive*, void*) -> int { return ARCHIVE_OK; };
    auto close_cb = [](struct archive*, void*) -> int { return ARCHIVE_OK; };
    auto write_cb = [](struct archive*, void* client, const void* buf, size_t len) -> la_ssize_t {
        auto* data = static_cast<CallbackData*>(client);
        int64_t result = (*data->fn)(buf, len);
        if (result < 0) return -1;
        return static_cast<la_ssize_t>(result);
    };

    if (archive_write_open(a, &cb_data, open_cb, write_cb, close_cb) != ARCHIVE_OK) {
        std::string err = archive_error_string(a);
        archive_write_free(a);
        throw std::runtime_error("Failed to open tar stream: " + err);
    }

    struct archive_entry* entry = archive_entry_new();

    for (const auto& rel_path : files) {
        fs::path full_path = base_dir / rel_path;

        if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) continue;

        auto file_size = fs::file_size(full_path);
        auto mtime = fs::last_write_time(full_path);
        auto mtime_sec = std::chrono::duration_cast<std::chrono::seconds>(
            mtime.time_since_epoch()).count();

        archive_entry_clear(entry);
        archive_entry_set_pathname(entry, rel_path.c_str());
        archive_entry_set_size(entry, static_cast<int64_t>(file_size));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, mtime_sec, 0);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            continue;
        }

        std::ifstream in(full_path, std::ios::binary);
        if (!in) continue;

        char buf[65536];
        while (in) {
            in.read(buf, sizeof(buf));
            auto bytes_read = in.gcount();
            if (bytes_read > 0) {
                if (archive_write_data(a, buf, static_cast<size_t>(bytes_read)) < 0) {
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    throw std::runtime_error("Tar stream write failed");
                }
            }
        }
    }

    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);
}

} // namespace platform
