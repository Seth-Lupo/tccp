#include "test_fixture.hpp"
#include <fstream>

// Fixture that adds requirements.txt to the project
class PipTest : public ClusterTest {
protected:
    void add_requirements(const std::string& contents) {
        std::ofstream f((project_dir_ / "requirements.txt").string());
        f << contents;
    }
};

// ── pip install runs and creates stamp file ──────────────────── ~5 min
TEST_F(PipTest, PipInstallCreatesStamp) {
    connect();

    // Add a small, fast-installing package
    add_requirements("six\n");

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->compute_node.empty());

    std::string user = cluster_user();
    std::string venv_path = "/tmp/" + user + "/" + project_name_ + "/.tccp-env/default/venv";

    // Stamp file should exist with v2: prefix
    auto check = run_on_compute(tj->compute_node,
        "cat " + venv_path + "/.req_installed 2>/dev/null || echo NOSTAMP");
    ASSERT_EQ(check.exit_code, 0);
    std::string stamp = check.stdout_data;
    while (!stamp.empty() && stamp.back() == '\n') stamp.pop_back();
    EXPECT_TRUE(stamp.find("v2:") == 0)
        << "Stamp file should start with 'v2:', got: '" << stamp << "'";

    wait_job_completed("quick", 120);
}

// ── pip install skipped when requirements unchanged ──────────── ~9 min
TEST_F(PipTest, PipInstallSkippedWhenUnchanged) {
    connect();

    add_requirements("six\n");

    // First run — installs packages
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string node = tj1->compute_node;
    std::string user = cluster_user();
    std::string venv_path = "/tmp/" + user + "/" + project_name_ + "/.tccp-env/default/venv";

    // Record stamp
    auto stamp1 = run_on_compute(node,
        "cat " + venv_path + "/.req_installed 2>/dev/null");
    ASSERT_EQ(stamp1.exit_code, 0);

    service_->prune_old_jobs();

    // Second run — requirements unchanged, should skip
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);

    // Stamp should be identical (same hash = same requirements)
    auto stamp2 = run_on_compute(tj2->compute_node,
        "cat " + venv_path + "/.req_installed 2>/dev/null");
    ASSERT_EQ(stamp2.exit_code, 0);
    EXPECT_EQ(stamp1.stdout_data, stamp2.stdout_data)
        << "Stamp changed despite requirements being unchanged";
}

// ── pip re-installs when requirements change ─────────────────── ~9 min
TEST_F(PipTest, PipReinstallsWhenRequirementsChange) {
    connect();

    add_requirements("six\n");

    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string user = cluster_user();
    std::string venv_path = "/tmp/" + user + "/" + project_name_ + "/.tccp-env/default/venv";

    auto stamp1 = run_on_compute(tj1->compute_node,
        "cat " + venv_path + "/.req_installed 2>/dev/null");

    service_->prune_old_jobs();

    // Change requirements
    add_requirements("six\nchardet\n");

    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    auto* tj2 = service_->find_job_by_name("quick");
    ASSERT_NE(tj2, nullptr);

    auto stamp2 = run_on_compute(tj2->compute_node,
        "cat " + venv_path + "/.req_installed 2>/dev/null");
    ASSERT_EQ(stamp2.exit_code, 0);
    EXPECT_NE(stamp1.stdout_data, stamp2.stdout_data)
        << "Stamp should change when requirements.txt changes";

    // New package should be installed
    std::string sif = "/cluster/home/" + user + "/tccp/container-cache/images/python_3.11-slim.sif";
    auto pip_check = run_on_compute(tj2->compute_node,
        "module load singularity 2>/dev/null || module load apptainer 2>/dev/null || true; "
        "singularity exec --bind " + venv_path + ":" + venv_path + " " + sif +
        " " + venv_path + "/bin/pip show chardet 2>/dev/null | grep -c Name || echo 0");
    ASSERT_EQ(pip_check.exit_code, 0);
    std::string found = pip_check.stdout_data;
    while (!found.empty() && found.back() == '\n') found.pop_back();
    EXPECT_EQ(found, "1") << "chardet not installed after requirements change";
}

// ── No requirements.txt = no stamp file ──────────────────────── ~5 min
TEST_F(PipTest, NoRequirementsNoStamp) {
    connect();

    // Don't add requirements.txt — just run with the default project files
    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);

    std::string user = cluster_user();
    std::string venv_path = "/tmp/" + user + "/" + project_name_ + "/.tccp-env/default/venv";

    auto check = run_on_compute(tj->compute_node,
        "test -f " + venv_path + "/.req_installed && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "MISSING")
        << "Stamp file should not exist when there is no requirements.txt";

    wait_job_completed("quick", 120);
}
