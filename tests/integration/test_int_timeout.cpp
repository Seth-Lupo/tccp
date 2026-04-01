#include "test_fixture.hpp"
#include <fstream>
#include <chrono>

// ── Allocation with insufficient time is skipped ──────────── ~5 min
TEST_F(ClusterTest, AllocationWithInsufficientTimeSkipped) {
    connect();

    // Run a job to create an allocation
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto allocs = service_->list_allocations();
    ASSERT_FALSE(allocs.empty());
    std::string old_alloc_id = allocs[0].slurm_id;
    service_->prune_old_jobs();

    // Manipulate state: set start_time far in the past so remaining < exp_time
    auto state = read_state_file();
    for (auto& a : state.allocations) {
        if (a.slurm_id == old_alloc_id) {
            // Set start time to 4 hours ago (allocation is 5min, so it's expired)
            a.start_time = "2020-01-01T00:00:00";
        }
    }
    write_state_file(state);

    // Reconnect to load manipulated state
    reconnect();

    // Run new job — old allocation should be skipped/reaped, new one created
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);
    EXPECT_TRUE(tj2->completed);
    EXPECT_EQ(tj2->exit_code, 0);

    // Should have a different allocation (old one expired)
    EXPECT_NE(tj2->slurm_id, old_alloc_id)
        << "Job ran on expired allocation instead of creating new one";
}

// ── Job completes normally with exit 0 (baseline) ─────────── ~5 min
TEST_F(ClusterTest, JobCompletesNormallyExitZero) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_TRUE(tj->completed);
    EXPECT_EQ(tj->exit_code, 0);
    EXPECT_FALSE(tj->canceled);
    EXPECT_TRUE(tj->init_error.empty());

    assert_state_consistent();
}

// ── Expired idle allocation reaped on reconnect ───────────── ~1 min
TEST_F(ClusterTest, ExpiredIdleAllocReaped) {
    connect();

    // Run a job to create an allocation
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto allocs = service_->list_allocations();
    ASSERT_FALSE(allocs.empty());
    std::string old_alloc_id = allocs[0].slurm_id;
    service_->prune_old_jobs();

    // Manipulate state: set start_time far in the past
    auto state = read_state_file();
    for (auto& a : state.allocations) {
        if (a.slurm_id == old_alloc_id) {
            a.start_time = "2020-01-01T00:00:00";
        }
    }
    write_state_file(state);

    // Reconnect — should reap the expired allocation
    reconnect();

    // Poll to trigger reaping
    service_->poll_jobs([](const TrackedJob&) {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    service_->poll_jobs([](const TrackedJob&) {});

    // Check state — expired allocation should be removed
    auto new_state = read_state_file();
    bool found_old = false;
    for (const auto& a : new_state.allocations) {
        if (a.slurm_id == old_alloc_id) {
            found_old = true;
            break;
        }
    }
    EXPECT_FALSE(found_old) << "Expired allocation not reaped from state";
}
