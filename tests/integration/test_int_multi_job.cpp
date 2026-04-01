#include "test_fixture.hpp"

TEST_F(ClusterTest, TwoJobsGetSeparateAllocations) {
    connect();

    // Run two jobs concurrently — they should each get their own allocation
    // since they'll both be active at the same time
    auto r1 = service_->run_job("slow");
    ASSERT_TRUE(r1.is_ok()) << "first run_job() failed: " << r1.error;

    // Wait for first to be running before starting second
    wait_job_running("slow", 300);

    // The "quick" job needs a separate allocation since "slow" is using its alloc
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << "second run_job() failed: " << r2.error;

    wait_job_running("quick", 300);

    auto* tj1 = service_->find_job_by_name("slow");
    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    ASSERT_NE(tj2, nullptr);

    // They should have different allocations (no double-claim)
    EXPECT_NE(tj1->slurm_id, tj2->slurm_id)
        << "Two concurrent jobs should not share the same allocation";

    auto allocs = service_->list_allocations();
    EXPECT_GE(allocs.size(), 2u) << "Expected at least 2 allocations";
}
