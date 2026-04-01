#include "test_fixture.hpp"
#include <fstream>

// ── Totally corrupted YAML, service recovers ─────────────────── ~10s
TEST_F(ClusterTest, CorruptStateFileRecovery) {
    connect();

    // Write garbage to state file
    auto state_path = service_->state_store()->path();
    {
        std::ofstream f(state_path.string());
        f << "{{{{totally broken yaml!!!!";
    }

    // Reconnect — should start fresh, not crash
    reconnect();

    EXPECT_TRUE(service_->is_connected());
    auto allocs = service_->list_allocations();
    EXPECT_TRUE(allocs.empty()) << "Corrupt state should produce empty allocations";
    assert_state_consistent();
}

// ── State file has allocations + jobs that don't exist ────────── ~15s
TEST_F(ClusterTest, PurelyFakeStatePrunedOnReconnect) {
    connect();

    // Inject completely fake state
    ProjectState fake;
    AllocationState a;
    a.slurm_id = "11111111";
    a.node = "fake-node-1";
    a.partition = "batch";
    a.active_job_id = "fake-job-1";
    fake.allocations.push_back(a);

    AllocationState a2;
    a2.slurm_id = "22222222";
    a2.node = "fake-node-2";
    a2.partition = "batch";
    fake.allocations.push_back(a2);

    JobState j;
    j.job_id = "fake-job-1";
    j.job_name = "quick";
    j.alloc_slurm_id = "11111111";
    j.compute_node = "fake-node-1";
    j.completed = false;
    j.init_complete = true;
    fake.jobs.push_back(j);

    write_state_file(fake);

    // Reconnect — reconcile should prune all fake allocations
    reconnect();

    auto allocs = service_->list_allocations();
    for (const auto& alloc : allocs) {
        EXPECT_NE(alloc.slurm_id, "11111111");
        EXPECT_NE(alloc.slurm_id, "22222222");
    }
    assert_state_consistent();
}

// ── State has a mix of real and fake allocations ──────────────── ~30s
TEST_F(ClusterTest, MixedRealFakeStatePrunedCorrectly) {
    connect();

    // Create a real allocation
    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string real_sid = result.value.slurm_id;

    // Inject a fake allocation into state alongside the real one
    auto state = read_state_file();
    AllocationState fake;
    fake.slurm_id = "77777777";
    fake.node = "ghost-node";
    fake.partition = "batch";
    state.allocations.push_back(fake);
    write_state_file(state);

    // Reconnect
    reconnect();

    // Real allocation should survive, fake should be pruned
    auto allocs = service_->list_allocations();
    bool real_found = false;
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, "77777777") << "Fake allocation survived";
        if (a.slurm_id == real_sid) real_found = true;
    }
    EXPECT_TRUE(real_found) << "Real allocation was incorrectly pruned";
    assert_state_consistent();
}

// ── Empty state file (zero bytes) ────────────────────────────── ~10s
TEST_F(ClusterTest, EmptyStateFileRecovery) {
    connect();

    auto state_path = service_->state_store()->path();
    {
        std::ofstream f(state_path.string());
        // Write nothing — zero-byte file
    }

    reconnect();
    EXPECT_TRUE(service_->is_connected());
    assert_state_consistent();
}

// ── State file deleted between operations ────────────────────── ~10s
TEST_F(ClusterTest, DeletedStateFileRecovery) {
    connect();

    auto state_path = service_->state_store()->path();
    if (fs::exists(state_path)) {
        fs::remove(state_path);
    }

    reconnect();
    EXPECT_TRUE(service_->is_connected());
    assert_state_consistent();
}
