#include "test_fixture.hpp"
#include <fstream>

// ── Cancel during sync of large project ───────────────────── ~30s-2min
TEST_F(ClusterTest, CancelDuringSyncLargeProject) {
    // Create many small files to make sync take longer
    for (int i = 0; i < 100; i++) {
        std::ofstream f((project_dir_ / ("file_" + std::to_string(i) + ".txt")).string());
        f << "content for file " << i << " with some padding data to increase size\n";
    }

    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << result.error;

    // Cancel ~1s after run (likely during sync/init)
    std::this_thread::sleep_for(std::chrono::seconds(1));
    service_->cancel_job("slow");

    std::this_thread::sleep_for(std::chrono::seconds(10));
    service_->poll_jobs([](const TrackedJob&) {});

    auto* tj = service_->find_job_by_name("slow");
    if (tj) {
        EXPECT_TRUE(tj->completed || tj->canceled || !tj->init_error.empty())
            << "Job not in terminal state after cancel during sync";
    }

    // Allocation should still exist (cancel doesn't deallocate)
    auto allocs = service_->list_allocations();
    EXPECT_FALSE(allocs.empty()) << "Allocation was unexpectedly deallocated after cancel";

    assert_state_consistent();
}

// ── Cancel after init complete ────────────────────────────── ~5 min
TEST_F(ClusterTest, CancelAfterInitComplete) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << result.error;

    wait_job_running("slow", 300);

    // Cancel immediately after init complete
    auto cancel_result = service_->cancel_job("slow");
    ASSERT_TRUE(cancel_result.is_ok()) << cancel_result.error;

    wait_job_completed("slow", 60);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(tj->completed);
    EXPECT_TRUE(tj->canceled);

    assert_state_consistent();
}

// ── Cancel and run new job on same allocation ─────────────── ~9 min
TEST_F(ClusterTest, CancelAndRunNewJob) {
    connect();

    auto r1 = service_->run_job("slow");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_running("slow", 300);

    // Record allocation
    auto allocs_before = service_->list_allocations();
    ASSERT_FALSE(allocs_before.empty());
    std::string alloc_id = allocs_before[0].slurm_id;

    // Cancel
    service_->cancel_job("slow");
    wait_job_completed("slow", 60);
    service_->prune_old_jobs();

    // Run new job — should reuse same allocation
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);
    EXPECT_TRUE(tj2->completed);
    EXPECT_EQ(tj2->exit_code, 0) << "Second job after cancel failed";

    // Verify same allocation was reused
    EXPECT_EQ(tj2->slurm_id, alloc_id) << "New allocation created instead of reusing";

    assert_state_consistent();
}
