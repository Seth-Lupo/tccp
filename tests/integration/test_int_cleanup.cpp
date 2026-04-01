#include "test_fixture.hpp"

TEST_F(ClusterTest, GracefulShutdownCancelsAllocations) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << "run_job() failed: " << result.error;

    wait_job_running("slow", 300);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    std::string sid = tj->slurm_id;

    // Cancel the running job first, then graceful shutdown releases the idle alloc
    service_->cancel_job("slow");
    wait_job_completed("slow", 60);

    service_->graceful_shutdown();

    // Give SLURM a moment to process the scancel
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Reconnect just to check SLURM state
    connect();
    EXPECT_FALSE(slurm_alloc_exists(sid))
        << "Allocation " << sid << " still exists after graceful shutdown";

    // Check no orphan allocations for this project
    auto check = service_->exec_remote(
        "squeue -u $USER -h -o '%j' 2>/dev/null | grep '" + project_name_ + "' | wc -l");
    if (check.exit_code == 0) {
        std::string count = check.stdout_data;
        while (!count.empty() && (count.back() == '\n' || count.back() == ' '))
            count.pop_back();
        EXPECT_EQ(count, "0") << "Orphan allocations found after shutdown";
    }
}
