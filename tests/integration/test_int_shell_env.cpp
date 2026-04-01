#include "test_fixture.hpp"
#include <fstream>

// ── Shell environment has correct working directory ──────────── ~5 min
TEST_F(ClusterTest, ShellEnvHasCorrectWorkdir) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->scratch_path.empty());

    std::string cmd = service_->job_manager()->shell_command(*tj, "pwd");
    auto check = run_on_compute(tj->compute_node, cmd);
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, tj->scratch_path) << "Working directory should be scratch path";

    wait_job_completed("quick", 120);
}

// ── Shell environment has custom YAML vars ───────────────────── ~5 min
TEST_F(ClusterTest, ShellEnvHasCustomVars) {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "environment:\n"
          << "  MY_VAR: test_value\n"
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

    std::string cmd = service_->job_manager()->shell_command(*tj, "echo $MY_VAR");
    auto check = run_on_compute(tj->compute_node, cmd);
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "test_value") << "Custom environment variable not set";

    wait_job_completed("quick", 120);
}

// ── Shell environment has Python venv ────────────────────────── ~5 min
TEST_F(ClusterTest, ShellEnvHasPythonVenv) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    std::string cmd = service_->job_manager()->shell_command(*tj, "which python");
    auto check = run_on_compute(tj->compute_node, cmd);
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_NE(out.find("venv"), std::string::npos)
        << "Python not from venv, got: " << out;

    wait_job_completed("quick", 120);
}
