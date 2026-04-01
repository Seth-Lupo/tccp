#include "test_fixture.hpp"
#include <fstream>

// ── Output file exists on scratch during job ──────────────── ~5 min
TEST_F(ClusterTest, OutputFileOnScratch) {
    // Script writes to output/
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output', exist_ok=True)\n"
          << "with open('output/result.txt', 'w') as f:\n"
          << "    f.write('test_output_data')\n"
          << "import time; time.sleep(5)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    // Wait a moment for the script to write
    std::this_thread::sleep_for(std::chrono::seconds(3));

    auto check = run_on_compute(tj->compute_node,
        "cat " + tj->scratch_path + "/output/result.txt 2>/dev/null || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "test_output_data") << "Output file not on scratch";

    wait_job_completed("quick", 120);
}

// ── Output downloaded locally after job completes ─────────── ~5 min
TEST_F(ClusterTest, OutputDownloadedLocally) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output', exist_ok=True)\n"
          << "with open('output/result.txt', 'w') as f:\n"
          << "    f.write('downloaded_content')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    // Extra poll to trigger try_return_output
    service_->poll_jobs([](const TrackedJob&) {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    service_->poll_jobs([](const TrackedJob&) {});

    // Check local output directory exists
    auto output_dir = project_dir_ / "output" / "quick";
    ASSERT_TRUE(fs::exists(output_dir)) << "output/quick/ not created locally";

    // Find the timestamped subdirectory
    bool found_result = false;
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.path().filename() == "result.txt") {
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            EXPECT_EQ(content, "downloaded_content");
            found_result = true;
            break;
        }
    }
    EXPECT_TRUE(found_result) << "result.txt not found in local output directory";
}

// ── Output latest symlink ─────────────────────────────────── ~5 min
TEST_F(ClusterTest, OutputLatestSymlink) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output', exist_ok=True)\n"
          << "with open('output/result.txt', 'w') as f:\n"
          << "    f.write('symlink_test')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    service_->poll_jobs([](const TrackedJob&) {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    service_->poll_jobs([](const TrackedJob&) {});

    auto latest = project_dir_ / "output" / "quick" / "latest";
    EXPECT_TRUE(fs::exists(latest) || fs::is_symlink(latest))
        << "output/quick/latest symlink not created";
}

// ── Nested output directories preserved ───────────────────── ~5 min
TEST_F(ClusterTest, OutputNestedDirPreserved) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output/sub', exist_ok=True)\n"
          << "with open('output/sub/nested.txt', 'w') as f:\n"
          << "    f.write('nested_content')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    service_->poll_jobs([](const TrackedJob&) {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    service_->poll_jobs([](const TrackedJob&) {});

    auto output_dir = project_dir_ / "output" / "quick";
    bool found_nested = false;
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.path().filename() == "nested.txt") {
            // Verify the parent path includes "sub"
            EXPECT_NE(entry.path().parent_path().filename().string().find("sub"),
                       std::string::npos)
                << "Nested directory structure not preserved";
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            EXPECT_EQ(content, "nested_content");
            found_nested = true;
            break;
        }
    }
    EXPECT_TRUE(found_nested) << "output/sub/nested.txt not found in local output";
}
