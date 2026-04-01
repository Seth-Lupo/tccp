#include "test_fixture.hpp"
#include <fstream>

// ── .env file synced to scratch (bypasses gitignore) ──────── ~5 min
TEST_F(ClusterTest, EnvFileSyncedToScratch) {
    // Create .env in project dir before connecting
    {
        std::ofstream f((project_dir_ / ".env").string());
        f << "SECRET_KEY=test123\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->compute_node.empty());
    ASSERT_FALSE(tj->scratch_path.empty());

    auto check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/.env && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << ".env file not found on compute scratch";

    wait_job_completed("quick", 120);
}

// ── .env in .gitignore still synced ───────────────────────── ~5 min
TEST_F(ClusterTest, GitignoredEnvStillSynced) {
    // Create .gitignore that excludes .env
    {
        std::ofstream f((project_dir_ / ".gitignore").string());
        f << ".env\n";
    }
    // Create .env
    {
        std::ofstream f((project_dir_ / ".env").string());
        f << "DB_HOST=localhost\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    auto check = run_on_compute(tj->compute_node,
        "test -f " + tj->scratch_path + "/.env && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << ".env should be synced even when gitignored";

    wait_job_completed("quick", 120);
}

// ── YAML environment variables exported in job ────────────── ~5 min
TEST_F(ClusterTest, YamlEnvVarsExported) {
    // Overwrite tccp.yaml with environment vars
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "environment:\n"
          << "  TCCP_TEST_VAR: hello123\n"
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
    // Script that prints the env var
    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "import os\n"
          << "print('ENVCHECK:' + os.environ.get('TCCP_TEST_VAR', 'MISSING'))\n"
          << "import time; time.sleep(3)\n";
    }

    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    // Read the log to check env var
    auto check = run_on_compute(tj->compute_node,
        "grep 'ENVCHECK:' " + tj->scratch_path + "/tccp.log 2>/dev/null || echo 'NO_MATCH'");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_NE(out, "NO_MATCH") << "ENVCHECK line not found in log";
    EXPECT_NE(out.find("hello123"), std::string::npos)
        << "TCCP_TEST_VAR not exported correctly, got: " << out;
}
