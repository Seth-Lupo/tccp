#include <gtest/gtest.h>
#include <managers/gitignore.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class GitignoreTest : public ::testing::Test {
protected:
    fs::path test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "tccp_gitignore_test";
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    void write_file(const std::string& rel_path, const std::string& content = "") {
        auto full = test_dir / rel_path;
        fs::create_directories(full.parent_path());
        std::ofstream(full) << content;
    }

    void write_gitignore(const std::string& content) {
        std::ofstream(test_dir / ".gitignore") << content;
    }
};

TEST_F(GitignoreTest, DefaultPatterns) {
    GitignoreParser parser(test_dir);

    // Default patterns should be ignored
    EXPECT_TRUE(parser.is_ignored(std::string(".git/")));
    EXPECT_TRUE(parser.is_ignored(std::string("__pycache__/")));
    EXPECT_TRUE(parser.is_ignored(std::string(".DS_Store")));
    EXPECT_TRUE(parser.is_ignored(std::string("foo.pyc")));
}

TEST_F(GitignoreTest, CustomPatterns) {
    write_gitignore("*.log\ndata/\n");
    GitignoreParser parser(test_dir);

    EXPECT_TRUE(parser.is_ignored(std::string("output.log")));
    EXPECT_TRUE(parser.is_ignored(std::string("data/")));
}

TEST_F(GitignoreTest, NegationPattern) {
    write_gitignore("*.log\n!important.log\n");
    GitignoreParser parser(test_dir);

    EXPECT_TRUE(parser.is_ignored(std::string("debug.log")));
    EXPECT_FALSE(parser.is_ignored(std::string("important.log")));
}

TEST_F(GitignoreTest, CollectFiles) {
    write_file("main.py", "print('hello')");
    write_file("utils.py", "# utils");
    write_file("output.pyc", "compiled");

    GitignoreParser parser(test_dir);
    auto files = parser.collect_files();

    // .pyc should be excluded by default patterns
    bool has_main = false, has_utils = false, has_pyc = false;
    for (const auto& f : files) {
        if (f.filename() == "main.py") has_main = true;
        if (f.filename() == "utils.py") has_utils = true;
        if (f.filename() == "output.pyc") has_pyc = true;
    }
    EXPECT_TRUE(has_main);
    EXPECT_TRUE(has_utils);
    EXPECT_FALSE(has_pyc);
}

TEST_F(GitignoreTest, CommentAndEmptyLines) {
    write_gitignore("# This is a comment\n\n*.tmp\n");
    GitignoreParser parser(test_dir);

    EXPECT_TRUE(parser.is_ignored(std::string("scratch.tmp")));
}
