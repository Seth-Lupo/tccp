#include "test_fixture.hpp"
#include <fstream>

// ── Cache dir created on compute ──────────────────────────── ~5 min
TEST_F(ClusterTest, CacheDirCreatedOnCompute) {
    // Overwrite tccp.yaml with cache config
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "cache: cache\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->compute_node.empty());

    std::string user = cluster_user();
    auto check = run_on_compute(tj->compute_node,
        "test -d /tmp/" + user + "/" + project_name_ + "/.tccp-cache"
        " && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "Cache directory not created on compute node";

    wait_job_completed("quick", 120);
}

// ── Cache dir accessible inside job ───────────────────────── ~5 min
TEST_F(ClusterTest, CacheDirAccessibleInJob) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "cache: cache\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }
    // Script writes to cache dir
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('cache', exist_ok=True)\n"
          << "with open('cache/marker.txt', 'w') as f:\n"
          << "    f.write('cache_test_ok')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    std::string user = cluster_user();
    auto check = run_on_compute(tj->compute_node,
        "cat /tmp/" + user + "/" + project_name_ + "/.tccp-cache/marker.txt 2>/dev/null || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "cache_test_ok") << "Cache marker file not written correctly";
}

// ── Cache dir shared across jobs on same allocation ───────── ~9 min
TEST_F(ClusterTest, CacheDirSharedAcrossJobs) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "cache: cache\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    // First job: write to cache
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "os.makedirs('cache', exist_ok=True)\n"
          << "with open('cache/shared', 'w') as f:\n"
          << "    f.write('shared_data_42')\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);
    service_->prune_old_jobs();

    // Second job: read from cache
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "with open('cache/shared', 'r') as f:\n"
          << "    content = f.read()\n"
          << "print('CACHEREAD:' + content)\n"
          << "import time; time.sleep(3)\n";
    }

    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);

    auto check = run_on_compute(tj2->compute_node,
        "grep 'CACHEREAD:' " + tj2->scratch_path + "/tccp.log 2>/dev/null || echo 'NO_MATCH'");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_NE(out.find("shared_data_42"), std::string::npos)
        << "Cache data not shared across jobs, got: " << out;
}
