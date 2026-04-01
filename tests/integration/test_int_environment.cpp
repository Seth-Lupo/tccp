#include "test_fixture.hpp"

// ── Container image exists on NFS after job runs ─────────────── ~5 min
TEST_F(ClusterTest, ContainerImageExistsAfterJobRun) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    // Container SIF should be on NFS (visible from DTN)
    // python type uses python_3.11-slim.sif
    std::string user = cluster_user();
    auto check = run_on_dtn(
        "test -f /cluster/home/" + user + "/tccp/container-cache/images/python_3.11-slim.sif"
        " && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "Container SIF not found on NFS after job run";
}

// ── dtach binary exists after job runs ───────────────────────── ~5 min
TEST_F(ClusterTest, DtachBinaryExistsAfterJobRun) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_completed("quick", 300);

    std::string user = cluster_user();
    auto check = run_on_dtn(
        "test -x /cluster/home/" + user + "/tccp/bin/dtach && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "dtach binary not found after job run";
}

// ── Venv exists on compute node after job runs ───────────────── ~5 min
TEST_F(ClusterTest, VenvExistsOnComputeAfterJobRun) {
    connect();

    auto result = service_->run_job("quick");
    ASSERT_TRUE(result.is_ok()) << result.error;
    wait_job_running("quick", 300);

    auto* tj = service_->find_job_by_name("quick");
    ASSERT_NE(tj, nullptr);
    ASSERT_FALSE(tj->compute_node.empty());

    std::string user = cluster_user();
    // Venv python is a symlink to container python — use test -L
    auto check = run_on_compute(tj->compute_node,
        "test -L /tmp/" + user + "/" + project_name_ +
        "/.tccp-env/default/venv/bin/python && echo EXISTS || echo MISSING");
    ASSERT_EQ(check.exit_code, 0);
    std::string out = check.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "EXISTS") << "Venv python symlink not found on compute node";

    wait_job_completed("quick", 120);
}

// ── Environment persists across job runs (no re-pull) ────────── ~9 min
TEST_F(ClusterTest, EnvironmentReusedAcrossJobs) {
    connect();

    // First job creates environment
    auto r1 = service_->run_job("quick");
    ASSERT_TRUE(r1.is_ok()) << r1.error;
    wait_job_completed("quick", 300);

    auto* tj1 = service_->find_job_by_name("quick");
    ASSERT_NE(tj1, nullptr);
    std::string node = tj1->compute_node;

    // Note container mtime
    std::string user = cluster_user();
    auto mtime1 = run_on_dtn(
        "stat -c %Y /cluster/home/" + user + "/tccp/container-cache/images/python_3.11-slim.sif 2>/dev/null");

    service_->prune_old_jobs();

    // Second job should reuse container (no re-pull)
    auto r2 = service_->run_job("quick");
    ASSERT_TRUE(r2.is_ok()) << r2.error;
    wait_job_completed("quick", 300);

    // Container mtime should be unchanged (not re-pulled)
    auto mtime2 = run_on_dtn(
        "stat -c %Y /cluster/home/" + user + "/tccp/container-cache/images/python_3.11-slim.sif 2>/dev/null");
    EXPECT_EQ(mtime1.stdout_data, mtime2.stdout_data)
        << "Container was re-pulled between job runs (mtime changed)";
}
