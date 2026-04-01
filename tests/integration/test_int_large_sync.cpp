#include "test_fixture.hpp"
#include <fstream>
#include <fmt/format.h>

// ── 1000 files sync correctly ────────────────────────────────── ~10 min
TEST_F(ClusterTest, ThousandFilesSync) {
    // Create 1000 small files
    for (int i = 0; i < 1000; i++) {
        std::ofstream f((project_dir_ / fmt::format("file_{:04d}.txt", i)).string());
        f << "content " << i << "\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    // Count synced files (excluding generated files like tccp_run.sh, .log, .sock, output/)
    auto check = run_on_compute(tj->compute_node,
        "find " + tj->scratch_path + " -maxdepth 1 -name 'file_*.txt' -type f | wc -l",
        60);
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    EXPECT_EQ(out, "1000") << "Expected 1000 synced files, got " << out;

    wait_job_completed("quick", 120);
}

// ── Nested directory tree synced correctly ────────────────────── ~10 min
TEST_F(ClusterTest, NestedDirTreeSync) {
    // 50 dirs × 3 levels × 5 files = 250 files
    for (int d = 0; d < 50; d++) {
        auto dir = project_dir_ / fmt::format("dir_{:02d}", d);
        fs::create_directories(dir / "sub1" / "sub2");
        for (int f = 0; f < 5; f++) {
            std::string fname = fmt::format("data_{}.txt", f);
            // Level 1
            { std::ofstream o((dir / fname).string()); o << "L1 " << d << " " << f << "\n"; }
            // Level 2 - put in sub1 only for some
            if (f < 3) {
                std::ofstream o((dir / "sub1" / fname).string()); o << "L2 " << d << " " << f << "\n";
            }
            // Level 3 - put in sub1/sub2 only for some
            if (f < 2) {
                std::ofstream o((dir / "sub1" / "sub2" / fname).string()); o << "L3 " << d << " " << f << "\n";
            }
        }
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Spot-check 10 paths
    std::vector<std::string> checks = {
        "dir_00/data_0.txt",
        "dir_00/sub1/data_0.txt",
        "dir_00/sub1/sub2/data_0.txt",
        "dir_25/data_4.txt",
        "dir_25/sub1/data_2.txt",
        "dir_49/data_0.txt",
        "dir_49/sub1/sub2/data_1.txt",
        "dir_10/data_3.txt",
        "dir_30/sub1/data_1.txt",
        "dir_40/sub1/sub2/data_0.txt",
    };

    for (const auto& path : checks) {
        auto c = run_on_compute(tj->compute_node,
            "test -f " + tj->scratch_path + "/" + path + " && echo OK || echo MISSING");
        ASSERT_EQ(c.exit_code, 0);
        std::string o = c.stdout_data;
        while (!o.empty() && o.back() == '\n') o.pop_back();
        EXPECT_EQ(o, "OK") << "Missing: " << path;
    }

    wait_job_completed("quick", 120);
}

// ── Incremental resync with modifications ────────────────────── ~15 min
TEST_F(ClusterTest, IncrementalResyncSubset) {
    // Create 500 files
    for (int i = 0; i < 500; i++) {
        std::ofstream f((project_dir_ / fmt::format("inc_{:04d}.txt", i)).string());
        f << "original " << i << "\n";
    }

    connect();

    // First run
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);
    service_->prune_old_jobs();

    // Modify 10 files
    for (int i = 0; i < 10; i++) {
        std::ofstream f((project_dir_ / fmt::format("inc_{:04d}.txt", i)).string());
        f << "modified " << i << "\n";
    }
    // Add 5 new files
    for (int i = 500; i < 505; i++) {
        std::ofstream f((project_dir_ / fmt::format("inc_{:04d}.txt", i)).string());
        f << "new " << i << "\n";
    }
    // Delete 5 files
    for (int i = 495; i < 500; i++) {
        fs::remove(project_dir_ / fmt::format("inc_{:04d}.txt", i));
    }

    // Second run — should apply incremental diff
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Verify modified file has new content
    auto mod_check = run_on_compute(tj->compute_node,
        "grep 'modified 5' " + tj->scratch_path + "/inc_0005.txt >/dev/null 2>&1"
        " && echo FOUND || echo MISSING");
    ASSERT_EQ(mod_check.exit_code, 0);
    std::string mout = mod_check.stdout_data;
    while (!mout.empty() && mout.back() == '\n') mout.pop_back();
    EXPECT_EQ(mout, "FOUND") << "Modified file not updated on resync";

    // Verify new file exists
    auto new_check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/inc_0502.txt && echo OK || echo MISSING");
    ASSERT_EQ(new_check.exit_code, 0);
    std::string nout = new_check.stdout_data;
    while (!nout.empty() && nout.back() == '\n') nout.pop_back();
    EXPECT_EQ(nout, "OK") << "New file not added on resync";

    // Verify deleted file is gone
    auto del_check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/inc_0497.txt && echo EXISTS || echo GONE");
    ASSERT_EQ(del_check.exit_code, 0);
    std::string dout = del_check.stdout_data;
    while (!dout.empty() && dout.back() == '\n') dout.pop_back();
    EXPECT_EQ(dout, "GONE") << "Deleted file still present after resync";

    wait_job_completed("quick", 120);
}
