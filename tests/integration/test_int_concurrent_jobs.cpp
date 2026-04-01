#include "test_fixture.hpp"
#include <fstream>

// ── Three jobs run concurrently ──────────────────────────────── ~15 min
TEST_F(ClusterTest, ThreeJobsRunConcurrently) {
    // Write config with 3 job definitions
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "jobs:\n"
          << "  job_a:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n"
          << "  job_b:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n"
          << "  job_c:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    connect();

    auto r1 = service_->run_job("job_a");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_running("job_a", 300);

    auto r2 = service_->run_job("job_b");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_running("job_b", 300);

    auto r3 = service_->run_job("job_c");
    ASSERT_TRUE(r3.is_ok()) << r3.error;
    wait_job_running("job_c", 300);

    auto* tj1 = service_->find_job_by_name("job_a");
    auto* tj2 = service_->find_job_by_name("job_b");
    auto* tj3 = service_->find_job_by_name("job_c");
    ASSERT_NE(tj1, nullptr);
    ASSERT_NE(tj2, nullptr);
    ASSERT_NE(tj3, nullptr);

    // All 3 should have separate allocations
    EXPECT_NE(tj1->slurm_id, tj2->slurm_id);
    EXPECT_NE(tj1->slurm_id, tj3->slurm_id);
    EXPECT_NE(tj2->slurm_id, tj3->slurm_id);

    // All should have compute nodes
    EXPECT_FALSE(tj1->compute_node.empty());
    EXPECT_FALSE(tj2->compute_node.empty());
    EXPECT_FALSE(tj3->compute_node.empty());

    auto allocs = service_->list_allocations();
    EXPECT_GE(allocs.size(), 3u) << "Expected at least 3 allocations";
    assert_state_consistent();

    service_->cancel_job("job_a");
    service_->cancel_job("job_b");
    service_->cancel_job("job_c");
    wait_job_completed("job_a", 60);
    wait_job_completed("job_b", 60);
    wait_job_completed("job_c", 60);
}

// ── Concurrent jobs complete cleanly ─────────────────────────── ~10 min
TEST_F(ClusterTest, ConcurrentJobsCompleteCleanly) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n"
          << "  slow:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    connect();

    auto r1 = service_->run_job("slow");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_running("slow", 300);

    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;

    // Wait for quick to complete naturally
    wait_job_completed("quick", 300);

    auto* tjq = service_->find_job_by_name("quick");
    ASSERT_NE(tjq, nullptr);
    EXPECT_EQ(tjq->exit_code, 0);

    // Slow should still be running
    auto* tjs = service_->find_job_by_name("slow");
    ASSERT_NE(tjs, nullptr);
    EXPECT_TRUE(tjs->init_complete);
    EXPECT_FALSE(tjs->completed);

    assert_state_consistent();

    service_->cancel_job("slow");
    wait_job_completed("slow", 60);
    assert_state_consistent();
}

// ── Staggered completion doesn't corrupt state ───────────────── ~10 min
TEST_F(ClusterTest, StaggeredCompletionNoCorruption) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n"
          << "  slow:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    connect();

    // Run quick and slow concurrently
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    auto r2 = service_->run_job("slow");
    ASSERT_TRUE(r2.is_ok()) << r2.error;

    wait_job_running("quick", 300);
    wait_job_running("slow", 300);

    // Quick completes first
    wait_job_completed("quick", 300);

    // Slow should still be running
    auto* tjs = service_->find_job_by_name("slow");
    ASSERT_NE(tjs, nullptr);
    EXPECT_FALSE(tjs->completed) << "Slow job should still be running after quick completes";
    assert_state_consistent();

    service_->cancel_job("slow");
    wait_job_completed("slow", 60);
    assert_state_consistent();
}
