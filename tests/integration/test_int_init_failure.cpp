#include "test_fixture.hpp"
#include <fstream>

namespace {

void wait_init_error(TccpService& service, const std::string& job_name, int timeout_s = 300) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        service.poll_jobs([](const TrackedJob&) {});
        auto* tj = service.find_job_by_name(job_name);
        if (tj && !tj->init_error.empty()) return;
        if (tj && tj->completed) return;  // Also accept completed (script might fail fast)
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    FAIL() << "Timed out waiting for init error on job '" << job_name << "'";
}

} // namespace

// ── Invalid script fails cleanly ─────────────────────────────── ~5 min
TEST_F(ClusterTest, InvalidScriptFailsCleanly) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: nonexistent.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    connect();

    auto result = service_->run_job("quick");
    // May fail at run_job level (script not found locally) or during init
    if (result.is_ok()) {
        wait_init_error(*service_, "quick", 300);
        auto* tj = service_->find_job_by_name("quick");
        ASSERT_NE(tj, nullptr);
        // Should have an error — either init_error or completed with non-zero exit
        EXPECT_TRUE(!tj->init_error.empty() || (tj->completed && tj->exit_code != 0))
            << "Expected failure for nonexistent script";
    }
    // If run_job itself fails, that's also acceptable
    assert_state_consistent();
}

// ── Broken requirements fails cleanly ────────────────────────── ~10 min
TEST_F(ClusterTest, BrokenRequirementsFailsCleanly) {
    {
        std::ofstream f((project_dir_ / "requirements.txt").string());
        f << "totally-fake-package-xyz==99.99\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;

    wait_init_error(*service_, "quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(!tj->init_error.empty() || (tj->completed && tj->exit_code != 0))
        << "Expected failure for broken requirements";
    assert_state_consistent();
}

// ── Init failure does not leak allocation ────────────────────── ~15 min
TEST_F(ClusterTest, InitFailureDoesNotLeakAllocation) {
    {
        std::ofstream f((project_dir_ / "requirements.txt").string());
        f << "totally-fake-package-xyz==99.99\n";
    }

    connect();

    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;

    wait_init_error(*service_, "quick", 300);
    assert_state_consistent();

    // The allocation should still be tracked (not leaked)
    auto allocs = service_->list_allocations();
    EXPECT_GE(allocs.size(), 1u) << "Allocation should still exist after init failure";

    // Remove broken requirements and run a valid job on the same allocation
    fs::remove(project_dir_ / "requirements.txt");

    // Rewrite test_quick.py to ensure fresh
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "print('hello from tccp test')\nimport time; time.sleep(3)\n";
    }

    service_->prune_old_jobs();

    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_EQ(tj->exit_code, 0) << "Valid job should succeed after init failure";
    assert_state_consistent();
}
