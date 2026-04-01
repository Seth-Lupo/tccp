#include "test_fixture.hpp"
#include <fstream>

// ── Orphaned running job pruned on reconnect ─────────────────── ~10 min
TEST_F(ClusterTest, OrphanedRunningJobPrunedOnReconnect) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("slow", 300);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    std::string sid = tj->slurm_id;

    // Force-cancel the SLURM allocation (simulates external kill)
    force_cancel_all();
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Reconnect — should detect the job as completed/failed
    reconnect();

    // Poll to trigger reconciliation
    for (int i = 0; i < 10; i++) {
        service_->poll_jobs([](const TrackedJob&) {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // Allocation should be pruned
    EXPECT_FALSE(slurm_alloc_exists(sid)) << "SLURM alloc should be gone after scancel";
    assert_state_consistent();
}

// ── Stale running job in state reconciled ────────────────────── ~5 min
TEST_F(ClusterTest, StaleRunningJobInStateReconciled) {
    connect();

    // Write fake state with a non-existent allocation
    auto state = read_state_file();
    JobState fake;
    fake.job_id = "fake-stale-job";
    fake.job_name = "quick";
    fake.alloc_slurm_id = "999999999";
    fake.init_complete = true;
    fake.completed = false;
    state.jobs.push_back(fake);

    AllocationState fake_alloc;
    fake_alloc.slurm_id = "999999999";
    fake_alloc.active_job_id = "fake-stale-job";
    state.allocations.push_back(fake_alloc);
    write_state_file(state);

    // Reconnect — reconcile should prune the fake allocation
    reconnect();

    // Poll to detect stale state
    for (int i = 0; i < 5; i++) {
        service_->poll_jobs([](const TrackedJob&) {});
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // Fake allocation should be pruned (doesn't exist in SLURM)
    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, "999999999")
            << "Fake allocation survived reconciliation";
    }
    assert_state_consistent();
}

// ── Disconnect during run recovers ───────────────────────────── ~10 min
TEST_F(ClusterTest, DisconnectDuringRunRecovers) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    // Disconnect and reconnect
    reconnect();

    // Job should complete even after disconnect
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    EXPECT_EQ(tj->exit_code, 0) << "Quick job should still succeed after reconnect";
    assert_state_consistent();
}
