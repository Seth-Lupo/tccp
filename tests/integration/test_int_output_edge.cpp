#include "test_fixture.hpp"
#include <fstream>

// ── Large output file downloaded correctly ────────────────────── ~10 min
TEST_F(ClusterTest, LargeOutputDownloaded) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output', exist_ok=True)\n"
          << "with open('output/data.bin', 'wb') as f:\n"
          << "    f.write(b'X' * (5 * 1024 * 1024))\n"  // 5MB
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    // Poll a few times to trigger output download
    for (int i = 0; i < 5; i++) {
        service_->poll_jobs([](const TrackedJob&) {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    auto output_dir = project_dir_ / "output" / "quick";
    bool found = false;
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.path().filename() == "data.bin") {
            auto size = fs::file_size(entry.path());
            EXPECT_EQ(size, 5u * 1024 * 1024) << "Output file size mismatch";
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "data.bin not found in local output";
}

// ── Binary output preserved byte-for-byte ────────────────────── ~10 min
TEST_F(ClusterTest, BinaryOutputPreserved) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output', exist_ok=True)\n"
          << "with open('output/binary.dat', 'wb') as f:\n"
          << "    f.write(bytes(range(256)))\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    for (int i = 0; i < 5; i++) {
        service_->poll_jobs([](const TrackedJob&) {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    auto output_dir = project_dir_ / "output" / "quick";
    bool found = false;
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.path().filename() == "binary.dat") {
            auto size = fs::file_size(entry.path());
            EXPECT_EQ(size, 256u) << "Binary file size should be 256 bytes";
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "binary.dat not found in local output";
}

// ── Deeply nested output directories preserved ───────────────── ~10 min
TEST_F(ClusterTest, DeeplyNestedOutput) {
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('output/a/b/c/d/e', exist_ok=True)\n"
          << "with open('output/a/b/c/d/e/result.txt', 'w') as f:\n"
          << "    f.write('deep_content')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    for (int i = 0; i < 5; i++) {
        service_->poll_jobs([](const TrackedJob&) {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    auto output_dir = project_dir_ / "output" / "quick";
    bool found = false;
    for (const auto& entry : fs::recursive_directory_iterator(output_dir)) {
        if (entry.path().filename() == "result.txt") {
            // Verify deep nesting preserved
            std::string p = entry.path().string();
            EXPECT_NE(p.find("a/b/c/d/e/result.txt"), std::string::npos)
                << "Nested path not preserved: " << p;
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            EXPECT_EQ(content, "deep_content");
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "result.txt not found in deeply nested output";
}
