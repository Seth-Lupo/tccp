#include "test_fixture.hpp"

TEST_F(ClusterTest, RunQuickJobToCompletion) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << "run_job() failed: " << result.error;

    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(tj->completed);
    EXPECT_EQ(tj->exit_code, 0);

    // Allocation should be idle after completion
    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        if (a.slurm_id == tj->slurm_id) {
            EXPECT_EQ(a.status, "IDLE") << "Allocation not released after job completion";
        }
    }

    // State YAML should have the job marked completed
    auto state = read_state_file();
    bool found = false;
    for (const auto& j : state.jobs) {
        if (j.job_id == tj->job_id) {
            EXPECT_TRUE(j.completed);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Completed job not in state YAML";
}

TEST_F(ClusterTest, CancelRunningJob) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << "run_job() failed: " << result.error;

    wait_job_running("slow", 300);

    auto cancel_result = service_->cancel_job("slow");
    ASSERT_TRUE(cancel_result.is_ok()) << "cancel_job() failed: " << cancel_result.error;

    wait_job_completed("slow", 60);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(tj->completed);
    EXPECT_TRUE(tj->canceled);

    // Allocation should be idle after cancel
    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        if (a.slurm_id == tj->slurm_id) {
            EXPECT_EQ(a.status, "IDLE") << "Allocation not released after cancel";
        }
    }
}
