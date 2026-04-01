#include "test_fixture.hpp"

// ── Cancel job that doesn't exist ────────────────────────────── ~5s
TEST_F(ClusterTest, CancelNonexistentJob) {
    connect();

    auto result = service_->cancel_job("nonexistent");
    EXPECT_TRUE(result.is_err()) << "cancel of nonexistent job should fail";
    assert_state_consistent();
}

// ── Cancel already-completed job ─────────────────────────────── ~5 min
TEST_F(ClusterTest, CancelAlreadyCompletedJob) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok());
    wait_job_completed("quick", 300);

    // Job is done — canceling should be a no-op or error, not corrupt state
    auto cancel_result = service_->cancel_job("quick");
    // Don't assert success/failure — just ensure state is still clean
    assert_state_consistent();

    // Allocation should be idle (not stuck as BUSY)
    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        if (a.slurm_id == tj->slurm_id) {
            EXPECT_NE(a.status, "BUSY")
                << "Allocation stuck as BUSY after canceling completed job";
        }
    }
}

// ── Cancel during init (before job is running) ───────────────── ~30s
TEST_F(ClusterTest, CancelDuringInit) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok());

    // Cancel immediately (job is likely still INITIALIZING)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    service_->cancel_job("slow");

    // Wait for cancellation to take effect
    std::this_thread::sleep_for(std::chrono::seconds(10));
    service_->poll_jobs([](const TrackedJob&) {});

    auto* tj = service_->find_job_by_name("slow");
    if (tj) {
        // If the job was tracked, it should be marked done
        EXPECT_TRUE(tj->completed || tj->canceled || !tj->init_error.empty())
            << "Canceled-during-init job stuck in non-terminal state";
    }

    // The critical check: no allocation should be stuck as BUSY with a dead job
    assert_state_consistent();
}

// ── Rapid cancel after run ───────────────────────────────────── ~15s
TEST_F(ClusterTest, CancelImmediatelyAfterRun) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok());

    // Cancel within milliseconds of submitting
    service_->cancel_job("quick");
    std::this_thread::sleep_for(std::chrono::seconds(10));
    service_->poll_jobs([](const TrackedJob&) {});

    // State must be consistent regardless of race outcome
    assert_state_consistent();
}

// ── Double cancel ────────────────────────────────────────────── ~5 min
TEST_F(ClusterTest, DoubleCancelSameJob) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok());
    wait_job_running("slow", 300);

    // Cancel twice in quick succession
    service_->cancel_job("slow");
    service_->cancel_job("slow");

    std::this_thread::sleep_for(std::chrono::seconds(10));
    service_->poll_jobs([](const TrackedJob&) {});

    assert_state_consistent();
}
