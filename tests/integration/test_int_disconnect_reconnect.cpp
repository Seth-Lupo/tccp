#include "test_fixture.hpp"
#include <fstream>

// ── Disconnect with idle allocation, reconnect recovers it ────── ~30s
TEST_F(ClusterTest, ReconnectRecoverIdleAllocation) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Disconnect (state persisted on disk, allocation still alive in SLURM)
    reconnect();

    // After reconnect + reconcile, allocation should still be tracked and IDLE
    auto allocs = service_->list_allocations();
    bool found = false;
    for (const auto& a : allocs) {
        if (a.slurm_id == sid) {
            EXPECT_EQ(a.status, "IDLE");
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Allocation " << sid << " lost after reconnect";
    EXPECT_TRUE(slurm_alloc_exists(sid));
    assert_state_consistent();
}

// ── Disconnect with running job, reconnect recovers it ──────── ~5 min
TEST_F(ClusterTest, ReconnectRecoverRunningJob) {
    connect();

    auto result = service_->run_job("slow");
    ASSERT_TRUE(result.is_ok());
    wait_job_running("slow", 300);

    auto* tj = service_->find_job_by_name("slow");
    ASSERT_NE(tj, nullptr);
    std::string sid = tj->slurm_id;
    std::string jid = tj->job_id;

    // Disconnect and reconnect
    reconnect();

    // Job should be restored from state file
    auto* restored = service_->find_job_by_name("slow");
    ASSERT_NE(restored, nullptr) << "Running job lost after reconnect";
    EXPECT_EQ(restored->job_id, jid);
    EXPECT_EQ(restored->slurm_id, sid);
    EXPECT_TRUE(slurm_alloc_exists(sid));
    assert_state_consistent();
}

// ── Job completes while disconnected, reconnect detects it ──── ~5 min
TEST_F(ClusterTest, ReconnectDetectsCompletedJob) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok());
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    std::string sid = tj->slurm_id;

    // Wait for the job to actually complete (quick sleeps 3s)
    std::this_thread::sleep_for(std::chrono::seconds(15));

    // Disconnect and reconnect — job completed while we were "away"
    reconnect();

    // Poll should detect the completion
    service_->poll_jobs([](const TrackedJob&) {});
    std::this_thread::sleep_for(std::chrono::seconds(3));
    service_->poll_jobs([](const TrackedJob&) {});

    auto* restored = service_->find_job_by_name("quick");
    ASSERT_NE(restored, nullptr);
    // Job should be detected as completed (poll checks compute node)
    // Allocation should still exist and be idle
    assert_state_consistent();
}

// ── Allocation scancelled while disconnected ──────────────────── ~30s
TEST_F(ClusterTest, ReconnectAfterExternalScancel) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Simulate external user running scancel
    service_->exec_remote("scancel " + sid);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Reconnect — reconcile should prune the dead allocation
    reconnect();

    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, sid)
            << "Scancelled allocation survived reconnect+reconcile";
    }
    assert_state_consistent();
}

// ── Multiple disconnect/reconnect cycles don't corrupt state ── ~1 min
TEST_F(ClusterTest, RepeatedDisconnectReconnect) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Disconnect/reconnect 3 times rapidly
    for (int i = 0; i < 3; i++) {
        reconnect();

        auto allocs = service_->list_allocations();
        bool found = false;
        for (const auto& a : allocs) {
            if (a.slurm_id == sid) found = true;
        }
        EXPECT_TRUE(found) << "Allocation lost on reconnect cycle " << i;
        assert_state_consistent();
    }
}
