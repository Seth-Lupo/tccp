#include "test_fixture.hpp"
#include <fstream>

// ── Project files synced to compute scratch ──────────────────── ~5 min
TEST_F(ClusterTest, ProjectFilesSyncedToScratch) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->compute_node.empty());
    ASSERT_FALSE(tj->scratch_path.empty());

    // Check that our python scripts were synced
    auto check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/test_quick.py"
        " && test -f " + tj->scratch_path + "/test_slow.py"
        " && echo SYNCED || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "SYNCED") << "Project files not found on compute scratch";

    // tccp_run.sh should exist and be executable
    auto script_check = run_on_compute(tj->compute_node,
        "test -x " + tj->scratch_path + "/tccp_run.sh && echo OK || echo MISSING");
    ASSERT_EQ(script_check.exit_code, 0);
    std::string sout = script_check.stdout_data;
    while (!sout.empty() && sout.back() == '\n') sout.pop_back();
    EXPECT_EQ(sout, "OK") << "tccp_run.sh not found or not executable";

    // output/ dir should exist
    auto out_check = run_on_compute(tj->compute_node,
        "test -d " + tj->scratch_path + "/output && echo OK || echo MISSING");
    ASSERT_EQ(out_check.exit_code, 0);
    std::string oout = out_check.stdout_data;
    while (!oout.empty() && oout.back() == '\n') oout.pop_back();
    EXPECT_EQ(oout, "OK") << "output/ directory not created on scratch";

    wait_job_completed("quick", 120);
}

// ── Modified file propagates on re-sync ──────────────────────── ~9 min
TEST_F(ClusterTest, ModifiedFileSyncsToCompute) {
    connect();

    // First run
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string node = tj1->compute_node;
    service_->prune_old_jobs();

    // Modify a file locally
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "print('MODIFIED BY TEST')\nimport time; time.sleep(3)\n";
    }

    // Second run — should sync the modified file
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_running("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);
    ASSERT_FALSE(tj2->scratch_path.empty());

    // Verify the modified content arrived
    auto check = run_on_compute(tj2->compute_node,
        "grep 'MODIFIED BY TEST' " + tj2->scratch_path + "/test_quick.py >/dev/null 2>&1"
        " && echo FOUND || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string result_str = check.stdout_data;
    while (!result_str.empty() && result_str.back() == '\n') result_str.pop_back();
    EXPECT_EQ(result_str, "FOUND") << "Modified file content not found on compute node after re-sync";

    wait_job_completed("quick", 120);
}

// ── New file added locally appears on compute ────────────────── ~9 min
TEST_F(ClusterTest, NewFileAppearsAfterSync) {
    connect();

    // First run
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);
    service_->prune_old_jobs();

    // Add a new file locally
    {
        std::ofstream f((project_dir_ / "extra_data.txt").string());
        f << "test data from integration test\n";
    }

    // Second run
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_running("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);

    auto check = run_on_compute(tj2->compute_node,
        "test -f " + tj2->scratch_path + "/extra_data.txt && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "Newly added file not synced to compute node";

    wait_job_completed("quick", 120);
}

// ── Deleted file removed from compute on re-sync ─────────────── ~9 min
TEST_F(ClusterTest, DeletedFileRemovedAfterSync) {
    connect();

    // Create an extra file locally
    {
        std::ofstream f((project_dir_ / "to_delete.txt").string());
        f << "will be deleted\n";
    }

    // First run — syncs to_delete.txt
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string node = tj1->compute_node;
    service_->prune_old_jobs();

    // Delete the file locally
    fs::remove(project_dir_ / "to_delete.txt");

    // Second run — should remove it from scratch
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_running("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);

    auto check = run_on_compute(tj2->compute_node,
        "test -f " + tj2->scratch_path + "/to_delete.txt && echo EXISTS || echo GONE");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "GONE") << "Deleted file still on compute node after re-sync";

    wait_job_completed("quick", 120);
}

// ── Sync manifest saved in state after successful sync ──────── ~5 min
TEST_F(ClusterTest, SyncManifestPersistedInState) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto state = read_state_file();
    EXPECT_FALSE(state.last_sync_node.empty())
        << "last_sync_node not set after job run";
    EXPECT_FALSE(state.last_sync_scratch.empty())
        << "last_sync_scratch not set after job run";
    EXPECT_FALSE(state.last_sync_manifest.empty())
        << "last_sync_manifest empty after job run";

    // Manifest should contain our test files
    bool has_quick = false, has_slow = false;
    for (const auto& e : state.last_sync_manifest) {
        if (e.path == "test_quick.py") has_quick = true;
        if (e.path == "test_slow.py") has_slow = true;
        // Every entry should have valid md5 and positive size
        EXPECT_FALSE(e.md5.empty()) << "Manifest entry " << e.path << " has no MD5";
        EXPECT_GT(e.size, 0) << "Manifest entry " << e.path << " has zero size";
    }
    EXPECT_TRUE(has_quick) << "test_quick.py not in sync manifest";
    EXPECT_TRUE(has_slow) << "test_slow.py not in sync manifest";
}

// ── Special characters in filename ───────────────────────── ~5 min
TEST_F(ClusterTest, SpecialCharsInFilename) {
    {
        std::ofstream f((project_dir_ / "my-data file.txt").string());
        f << "special chars\n";
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    auto check = run_on_compute(tj->compute_node,
        "test -f \"" + tj->scratch_path + "/my-data file.txt\" && echo OK || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "OK") << "File with special chars not synced";

    wait_job_completed("quick", 120);
}

// ── .gitignore excludes files from sync ──────────────────── ~5 min
TEST_F(ClusterTest, GitignoreExcludesFiles) {
    // Create .gitignore that excludes *.log files
    {
        std::ofstream f((project_dir_ / ".gitignore").string());
        f << "*.log\n";
    }
    // Create a .log file that should be excluded
    {
        std::ofstream f((project_dir_ / "debug.log").string());
        f << "should not be synced\n";
    }
    // Create a normal file that should be synced
    {
        std::ofstream f((project_dir_ / "data.txt").string());
        f << "should be synced\n";
    }

    connect();
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // data.txt should be present
    auto data_check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/data.txt && echo OK || echo MISSING");
    ASSERT_EQ(data_check.exit_code, 0);
    std::string dout = data_check.stdout_data;
    while (!dout.empty() && dout.back() == '\n') dout.pop_back();
    EXPECT_EQ(dout, "OK") << "data.txt should be synced";

    // debug.log should NOT be present
    auto log_check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/debug.log && echo EXISTS || echo GONE");
    ASSERT_EQ(log_check.exit_code, 0);
    std::string lout = log_check.stdout_data;
    while (!lout.empty() && lout.back() == '\n') lout.pop_back();
    EXPECT_EQ(lout, "GONE") << "debug.log should be excluded by .gitignore";

    wait_job_completed("quick", 120);
}
