#include "test_fixture.hpp"
#include <fstream>

// ── dtach socket exists while job running ────────────────────── ~5 min
TEST_F(ClusterTest, DtachSocketExistsWhileRunning) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("slow", 300);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    auto check = run_on_compute(tj->compute_node,
        "test -e " + tj->scratch_path + "/tccp.sock && echo RUNNING || echo NOSOCK");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "RUNNING") << "dtach socket not found while job is running";

    service_->cancel_job("slow");
    wait_job_completed("slow", 60);
}

// ── dtach socket gone after job completes ────────────────────── ~5 min
TEST_F(ClusterTest, DtachSocketGoneAfterCompletion) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Give cleanup a moment
    std::this_thread::sleep_for(std::chrono::seconds(5));

    auto check = run_on_compute(tj->compute_node,
        "test -e " + tj->scratch_path + "/tccp.sock && echo STILL_THERE || echo GONE");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "GONE") << "dtach socket should be cleaned up after job completes";
}

// ── Job log file created on compute ──────────────────────────── ~5 min
TEST_F(ClusterTest, JobLogFileCreated) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    auto check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/tccp.log && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "tccp.log not found on compute node after job run";
}

// ── Job script output is correct ─────────────────────────────── ~5 min
TEST_F(ClusterTest, JobProducesExpectedOutput) {
    connect();

    // test_quick.py prints "hello from tccp test"
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // Check that the output appears in the job log
    auto check = run_on_compute(tj->compute_node,
        "grep -c 'hello from tccp test' " + tj->scratch_path + "/tccp.log 2>/dev/null || echo 0");
    ASSERT_EQ(check.exit_code, 0);
    std::string count = check.stdout_data;
    while (!count.empty() && count.back() == '\n') count.pop_back();
    EXPECT_NE(count, "0") << "Expected output not found in job log";
}

// ── Job exit code 0 for successful script ────────────────────── ~5 min
TEST_F(ClusterTest, SuccessfulJobExitCodeZero) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_EQ(tj->exit_code, 0) << "Quick job should exit with code 0";
}

// ── Failing script produces non-zero exit code ───────────────── ~5 min
TEST_F(ClusterTest, FailingJobExitCodeNonZero) {
    connect();

    // Write a script that exits with error
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import sys; sys.exit(42)\n";
    }

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(tj->completed);
    EXPECT_NE(tj->exit_code, 0) << "Failing script should have non-zero exit code";
}

// ── Run script contains correct working directory ────────────── ~5 min
TEST_F(ClusterTest, RunScriptHasCorrectWorkdir) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    // tccp_run.sh should cd to scratch dir
    auto check = run_on_compute(tj->compute_node,
        "grep -c 'cd " + tj->scratch_path + "' " + tj->scratch_path + "/tccp_run.sh 2>/dev/null || echo 0");
    ASSERT_EQ(check.exit_code, 0);
    std::string count = check.stdout_data;
    while (!count.empty() && count.back() == '\n') count.pop_back();
    EXPECT_NE(count, "0") << "Run script doesn't cd to scratch path";

    wait_job_completed("quick", 120);
}
