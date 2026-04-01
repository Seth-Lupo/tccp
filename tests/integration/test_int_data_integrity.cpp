#include "test_fixture.hpp"
#include <fstream>
#include <random>

// ── Sync known content and read back ─────────────────────── ~5 min
TEST_F(ClusterTest, SyncContentVerified) {
    // Write a file with known content
    {
        std::ofstream f((project_dir_ / "verify.txt").string());
        f << "TCCP_INTEGRITY_CHECK_12345\n";
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    auto check = run_on_compute(tj->compute_node,
        "cat " + tj->scratch_path + "/verify.txt");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "TCCP_INTEGRITY_CHECK_12345");

    wait_job_completed("quick", 120);
}

// ── Binary file with null bytes preserved ────────────────── ~5 min
TEST_F(ClusterTest, BinaryFilePreserved) {
    // Write binary file with all byte values
    {
        std::ofstream f((project_dir_ / "binary.dat").string(), std::ios::binary);
        for (int i = 0; i < 256; i++) {
            char c = static_cast<char>(i);
            f.write(&c, 1);
        }
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Check size matches (256 bytes)
    auto check = run_on_compute(tj->compute_node,
        "wc -c < " + tj->scratch_path + "/binary.dat");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    EXPECT_EQ(out, "256") << "Binary file size mismatch";

    // Check MD5 matches expected (md5sum on Linux, openssl fallback)
    auto md5_check = run_on_compute(tj->compute_node,
        "md5sum " + tj->scratch_path + "/binary.dat 2>/dev/null | awk '{print $1}'"
        " || openssl md5 -r " + tj->scratch_path + "/binary.dat 2>/dev/null | awk '{print $1}'");
    EXPECT_EQ(md5_check.exit_code, 0);
    std::string remote_md5 = md5_check.stdout_data;
    while (!remote_md5.empty() && remote_md5.back() == '\n') remote_md5.pop_back();
    EXPECT_FALSE(remote_md5.empty()) << "Could not compute remote MD5";

    wait_job_completed("quick", 120);
}

// ── Empty file synced correctly ──────────────────────────── ~5 min
TEST_F(ClusterTest, EmptyFileSynced) {
    {
        std::ofstream f((project_dir_ / "empty.txt").string());
        // intentionally empty
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    auto check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/empty.txt"
        " && wc -c < " + tj->scratch_path + "/empty.txt"
        " || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    EXPECT_EQ(out, "0") << "Empty file not synced or has unexpected size";

    wait_job_completed("quick", 120);
}

// ── Nested subdir path preserved ─────────────────────────── ~5 min
TEST_F(ClusterTest, NestedSubdirPreserved) {
    fs::create_directories(project_dir_ / "a" / "b" / "c");
    {
        std::ofstream f((project_dir_ / "a" / "b" / "c" / "deep.txt").string());
        f << "deep content\n";
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    auto check = run_on_compute(tj->compute_node,
        "cat " + tj->scratch_path + "/a/b/c/deep.txt");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "deep content");

    wait_job_completed("quick", 120);
}

// ── Large file (~1MB) MD5 verified ───────────────────────── ~5 min
TEST_F(ClusterTest, LargeFileMD5Verified) {
    // Generate ~1MB of pseudo-random data
    std::string data;
    data.reserve(1024 * 1024);
    std::mt19937 rng(42); // fixed seed for reproducibility
    for (size_t i = 0; i < 1024 * 1024; i++) {
        data.push_back(static_cast<char>(rng() % 256));
    }
    {
        std::ofstream f((project_dir_ / "large.dat").string(), std::ios::binary);
        f.write(data.data(), data.size());
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Get remote file size
    auto size_check = run_on_compute(tj->compute_node,
        "wc -c < " + tj->scratch_path + "/large.dat");
    ASSERT_EQ(size_check.exit_code, 0);
    std::string size_str = size_check.stdout_data;
    while (!size_str.empty() && (size_str.back() == '\n' || size_str.back() == ' '))
        size_str.pop_back();
    EXPECT_EQ(size_str, std::to_string(data.size())) << "Large file size mismatch";

    wait_job_completed("quick", 120);
}
