#include <gtest/gtest.h>
#include <platform/archive.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>

namespace fs = std::filesystem;

class ArchiveTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / ("tccp-archive-test-" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        if (fs::exists(tmp_dir_)) {
            fs::remove_all(tmp_dir_);
        }
    }

    // Write a file with arbitrary binary content
    void write_file(const std::string& rel_path, const std::string& content) {
        fs::path full = tmp_dir_ / "src" / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary);
        f.write(content.data(), content.size());
    }

    // Extract a tar file into extract_dir and return map of path -> content
    std::map<std::string, std::string> extract_tar(const fs::path& tar_path) {
        std::map<std::string, std::string> result;

        struct archive* a = archive_read_new();
        archive_read_support_format_tar(a);
        archive_read_support_filter_none(a);

        EXPECT_EQ(ARCHIVE_OK, archive_read_open_filename(a, tar_path.c_str(), 65536));

        struct archive_entry* entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::string name = archive_entry_pathname(entry);
            auto size = archive_entry_size(entry);
            std::string buf(size, '\0');
            if (size > 0) {
                archive_read_data(a, buf.data(), size);
            }
            result[name] = buf;
        }

        archive_read_close(a);
        archive_read_free(a);
        return result;
    }

    // Extract from a tar buffer (in memory)
    std::map<std::string, std::string> extract_tar_buffer(const std::string& buf) {
        std::map<std::string, std::string> result;

        struct archive* a = archive_read_new();
        archive_read_support_format_tar(a);
        archive_read_support_filter_none(a);

        EXPECT_EQ(ARCHIVE_OK, archive_read_open_memory(a, buf.data(), buf.size()));

        struct archive_entry* entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::string name = archive_entry_pathname(entry);
            auto size = archive_entry_size(entry);
            std::string data(size, '\0');
            if (size > 0) {
                archive_read_data(a, data.data(), size);
            }
            result[name] = data;
        }

        archive_read_close(a);
        archive_read_free(a);
        return result;
    }

    fs::path src_dir() { return tmp_dir_ / "src"; }
};

// ── create_tar round-trip ────────────────────────────────────

TEST_F(ArchiveTest, TextFileRoundTrip) {
    write_file("hello.txt", "Hello, world!\n");

    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {"hello.txt"});

    auto files = extract_tar(tar);
    ASSERT_EQ(files.count("hello.txt"), 1u);
    EXPECT_EQ(files["hello.txt"], "Hello, world!\n");
}

TEST_F(ArchiveTest, BinaryWithNullBytes) {
    std::string binary(256, '\0');
    for (int i = 0; i < 256; i++) binary[i] = static_cast<char>(i);

    write_file("bin.dat", binary);

    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {"bin.dat"});

    auto files = extract_tar(tar);
    ASSERT_EQ(files.count("bin.dat"), 1u);
    EXPECT_EQ(files["bin.dat"], binary);
}

TEST_F(ArchiveTest, EmptyFile) {
    write_file("empty.txt", "");

    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {"empty.txt"});

    auto files = extract_tar(tar);
    ASSERT_EQ(files.count("empty.txt"), 1u);
    EXPECT_EQ(files["empty.txt"], "");
}

TEST_F(ArchiveTest, NestedDirectories) {
    write_file("a/b/c/deep.txt", "deep content");

    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {"a/b/c/deep.txt"});

    auto files = extract_tar(tar);
    ASSERT_EQ(files.count("a/b/c/deep.txt"), 1u);
    EXPECT_EQ(files["a/b/c/deep.txt"], "deep content");
}

TEST_F(ArchiveTest, MultipleFiles) {
    write_file("a.txt", "aaa");
    write_file("b.txt", "bbb");
    write_file("sub/c.txt", "ccc");

    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {"a.txt", "b.txt", "sub/c.txt"});

    auto files = extract_tar(tar);
    EXPECT_EQ(files.size(), 3u);
    EXPECT_EQ(files["a.txt"], "aaa");
    EXPECT_EQ(files["b.txt"], "bbb");
    EXPECT_EQ(files["sub/c.txt"], "ccc");
}

TEST_F(ArchiveTest, NonExistentFileSkipped) {
    write_file("exists.txt", "ok");

    fs::path tar = tmp_dir_ / "out.tar";
    // Should not crash — just skips the missing file
    platform::create_tar(tar, src_dir(), {"exists.txt", "missing.txt"});

    auto files = extract_tar(tar);
    EXPECT_EQ(files.size(), 1u);
    EXPECT_EQ(files.count("exists.txt"), 1u);
}

TEST_F(ArchiveTest, EmptyFileList) {
    fs::path tar = tmp_dir_ / "out.tar";
    platform::create_tar(tar, src_dir(), {});

    // Should produce a valid (empty) tar
    EXPECT_TRUE(fs::exists(tar));
    auto files = extract_tar(tar);
    EXPECT_EQ(files.size(), 0u);
}

// ── create_tar_to_callback round-trip ────────────────────────

TEST_F(ArchiveTest, CallbackTextRoundTrip) {
    write_file("cb.txt", "callback content\n");

    std::string buf;
    platform::create_tar_to_callback(src_dir(), {"cb.txt"},
        [&buf](const void* data, size_t len) -> int64_t {
            buf.append(static_cast<const char*>(data), len);
            return static_cast<int64_t>(len);
        });

    auto files = extract_tar_buffer(buf);
    ASSERT_EQ(files.count("cb.txt"), 1u);
    EXPECT_EQ(files["cb.txt"], "callback content\n");
}

TEST_F(ArchiveTest, CallbackBinaryPreservesNulls) {
    std::string binary(1024, '\0');
    for (size_t i = 0; i < binary.size(); i++)
        binary[i] = static_cast<char>(i % 256);

    write_file("binary.dat", binary);

    std::string buf;
    platform::create_tar_to_callback(src_dir(), {"binary.dat"},
        [&buf](const void* data, size_t len) -> int64_t {
            buf.append(static_cast<const char*>(data), len);
            return static_cast<int64_t>(len);
        });

    auto files = extract_tar_buffer(buf);
    ASSERT_EQ(files.count("binary.dat"), 1u);
    EXPECT_EQ(files["binary.dat"], binary);
}
