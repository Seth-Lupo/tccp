#include "test_fixture.hpp"

TEST_F(ClusterTest, AllocationReusedForSecondJob) {
    connect();

    // Run first job
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << "first run_job() failed: " << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string first_slurm_id = tj1->slurm_id;

    // Prune so the name slot is free for reuse
    service_->prune_old_jobs();

    // Run second job with same name
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << "second run_job() failed: " << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);
    EXPECT_EQ(tj2->slurm_id, first_slurm_id)
        << "Expected allocation reuse, got new SLURM ID";

    // Should still be only 1 allocation
    auto allocs = service_->list_allocations();
    EXPECT_EQ(allocs.size(), 1u) << "Expected 1 allocation, got " << allocs.size();
}
