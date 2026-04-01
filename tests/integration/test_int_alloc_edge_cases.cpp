#include "test_fixture.hpp"
#include <fstream>

// ── Deallocate nonexistent allocation ────────────────────────── ~5s
TEST_F(ClusterTest, DeallocateNonexistent) {
    connect();
    // Should not crash or corrupt state
    service_->deallocate("99999999", nullptr);
    assert_state_consistent();
}

// ── Deallocate all when no allocations exist ─────────────────── ~5s
TEST_F(ClusterTest, DeallocateAllEmpty) {
    connect();
    service_->deallocate("", nullptr);
    assert_state_consistent();
    auto allocs = service_->list_allocations();
    EXPECT_TRUE(allocs.empty());
}

// ── Allocation with stale busy ref becomes claimable ─────────── ~1 min
TEST_F(ClusterTest, StaleAllocBecomesFreeForNewJob) {
    connect();

    // Create an allocation and manually mark it busy with a fake job
    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Inject stale busy ref
    auto state = read_state_file();
    for (auto& a : state.allocations) {
        if (a.slurm_id == sid) a.active_job_id = "dead-job-999";
    }
    write_state_file(state);

    // Reconnect — reconcile should clear stale ref, alloc becomes IDLE
    reconnect();

    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        if (a.slurm_id == sid) {
            EXPECT_EQ(a.status, "IDLE")
                << "Allocation with stale job ref should be IDLE after reconcile";
        }
    }

    // Should be able to run a new job that claims this allocation
    auto run_result = service_->run_job("quick");
    ASSERT_TRUE(run_result.is_ok()) << "Failed to run job on recovered allocation: " << run_result.error;
    wait_job_completed("quick", 300);

    assert_state_consistent();
}

// ── Multiple allocations, deallocate just the idle ones ──────── ~1 min
TEST_F(ClusterTest, DeallocateIdlePreservesBusy) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");

    // Create two allocations
    auto r1 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r1.is_ok());
    auto r2 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r2.is_ok());

    std::string sid1 = r1.value.slurm_id;
    std::string sid2 = r2.value.slurm_id;

    // Mark one as busy (simulate a job using it)
    am->assign_job(sid1, "fake-job-for-test");

    // Deallocate all idle — should only kill sid2
    service_->deallocate("", nullptr);

    auto allocs = service_->list_allocations();
    bool found_busy = false;
    for (const auto& a : allocs) {
        if (a.slurm_id == sid1) found_busy = true;
        EXPECT_NE(a.slurm_id, sid2)
            << "Idle allocation should have been deallocated";
    }
    EXPECT_TRUE(found_busy) << "Busy allocation was incorrectly deallocated";

    // Clean up: release the fake job so TearDown can deallocate
    am->release_job(sid1);
}

// ── Reconcile after SLURM restart (all allocations gone) ──── ~30s
TEST_F(ClusterTest, ReconcileAfterAllAllocsDead) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto r1 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r1.is_ok());
    auto r2 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r2.is_ok());

    // Kill both externally
    service_->exec_remote("scancel " + r1.value.slurm_id);
    service_->exec_remote("scancel " + r2.value.slurm_id);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    service_->reconcile_allocations();

    auto allocs = service_->list_allocations();
    EXPECT_TRUE(allocs.empty())
        << "Expected 0 allocations after all scancelled, got " << allocs.size();
    assert_state_consistent();
}
