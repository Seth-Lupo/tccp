#include "test_fixture.hpp"
#include <fstream>

TEST_F(ClusterTest, ScancelledAllocPrunedByReconcile) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Kill it directly via SLURM (bypass service)
    auto cancel_result = service_->exec_remote("scancel " + sid);
    EXPECT_EQ(cancel_result.exit_code, 0);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Reconcile should detect it's gone
    service_->reconcile_allocations();

    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, sid) << "Scancelled allocation survived reconcile";
    }
}

TEST_F(ClusterTest, FakeAllocPrunedByReconcile) {
    connect();

    // Inject a fake allocation directly into state YAML
    auto state = read_state_file();
    AllocationState fake;
    fake.slurm_id = "99999999";
    fake.node = "fake-node";
    fake.partition = "batch";
    state.allocations.push_back(fake);

    StateStore store(project_name_);
    store.save(state);

    // Reconcile should prune the fake
    service_->reconcile_allocations();

    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, "99999999") << "Fake allocation survived reconcile";
    }

    // State YAML should also be clean
    auto cleaned_state = read_state_file();
    for (const auto& a : cleaned_state.allocations) {
        EXPECT_NE(a.slurm_id, "99999999") << "Fake allocation in state YAML after reconcile";
    }
}

TEST_F(ClusterTest, StaleJobRefClearedByReconcile) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    // Inject a fake active_job_id into the allocation state
    auto state = read_state_file();
    for (auto& a : state.allocations) {
        if (a.slurm_id == sid) {
            a.active_job_id = "nonexistent-job-id";
        }
    }
    StateStore store(project_name_);
    store.save(state);

    // Reconcile should clear the stale job ref
    service_->reconcile_allocations();

    auto cleaned_state = read_state_file();
    for (const auto& a : cleaned_state.allocations) {
        if (a.slurm_id == sid) {
            EXPECT_TRUE(a.active_job_id.empty())
                << "Stale job ref not cleared: " << a.active_job_id;
        }
    }
}

TEST_F(ClusterTest, NoStaleRefsAfterReconcile) {
    connect();

    // Allocate and deallocate to create some state churn
    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto r1 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r1.is_ok());
    service_->deallocate(r1.value.slurm_id, nullptr);

    // Allocate another
    auto r2 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r2.is_ok());

    service_->reconcile_allocations();

    // Every allocation in state should exist in SLURM
    auto state = read_state_file();
    for (const auto& a : state.allocations) {
        EXPECT_TRUE(slurm_alloc_exists(a.slurm_id))
            << "State references non-existent SLURM alloc: " << a.slurm_id;
    }
}
