#include "test_fixture.hpp"

TEST_F(ClusterTest, AllocateAndList) {
    connect();

    auto* am = service_->alloc_manager();
    ASSERT_NE(am, nullptr);

    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok()) << "allocate() failed: " << result.error;

    auto allocs = service_->list_allocations();
    ASSERT_GE(allocs.size(), 1u);

    // Find our allocation
    bool found = false;
    for (const auto& a : allocs) {
        if (a.slurm_id == result.value.slurm_id) {
            EXPECT_EQ(a.status, "IDLE");
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Allocation not found in list_allocations()";

    // Verify in state YAML
    auto state = read_state_file();
    bool in_state = false;
    for (const auto& a : state.allocations) {
        if (a.slurm_id == result.value.slurm_id) in_state = true;
    }
    EXPECT_TRUE(in_state) << "Allocation not in state YAML";

    // Verify in SLURM
    EXPECT_TRUE(slurm_alloc_exists(result.value.slurm_id));
}

TEST_F(ClusterTest, ReconcileKeepsLiveAllocation) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());

    service_->reconcile_allocations();
    auto allocs = service_->list_allocations();
    bool found = false;
    for (const auto& a : allocs) {
        if (a.slurm_id == result.value.slurm_id) found = true;
    }
    EXPECT_TRUE(found) << "Live allocation pruned by reconcile";
}

TEST_F(ClusterTest, DeallocateRemovesFromSlurmAndState) {
    connect();

    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto result = am->allocate(profile, nullptr);
    ASSERT_TRUE(result.is_ok());
    std::string sid = result.value.slurm_id;

    service_->deallocate(sid, nullptr);

    // Should be gone from list
    auto allocs = service_->list_allocations();
    for (const auto& a : allocs) {
        EXPECT_NE(a.slurm_id, sid) << "Deallocated allocation still in list";
    }

    // Should be gone from state YAML
    auto state = read_state_file();
    for (const auto& a : state.allocations) {
        EXPECT_NE(a.slurm_id, sid) << "Deallocated allocation still in state YAML";
    }

    // Should be gone from SLURM (may take a moment)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_FALSE(slurm_alloc_exists(sid));
}
