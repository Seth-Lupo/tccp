#pragma once

#include <gtest/gtest.h>
#include <managers/tccp_service.hpp>
#include <managers/state_store.hpp>
#include <filesystem>
#include <string>
#include <atomic>

namespace fs = std::filesystem;

// Base fixture for cluster integration tests.
// Gated behind TCCP_INTEGRATION env var — tests skip if not set.
// Each test gets a unique project dir with tccp.yaml + test scripts.
class ClusterTest : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    // Connect the service (loads config from project_dir_)
    void connect();

    // Disconnect and reconnect (fresh service, reloads state from disk)
    void reconnect();

    // Poll until a job reaches a target status or timeout
    void wait_job_running(const std::string& job_name, int timeout_s = 120);
    void wait_job_completed(const std::string& job_name, int timeout_s = 120);

    // Query SLURM directly for an allocation
    bool slurm_alloc_exists(const std::string& slurm_id);

    // Read state file directly (bypass service)
    ProjectState read_state_file();

    // Write state file directly (bypass service)
    void write_state_file(const ProjectState& state);

    // Cancel all test allocations via exec_remote (cleanup helper)
    void force_cancel_all();

    // Clean remote project dir and scratch dirs for this test project
    void clean_remote();

    // Assert no bad state: every local allocation exists in SLURM,
    // no allocation has a stale active_job_id, no orphan SLURM jobs
    void assert_state_consistent();

    // Run a command on the DTN node (primary shell channel, has Kerberos)
    SSHResult run_on_dtn(const std::string& command, int timeout_secs = 30);

    // Run a command on a compute node (SSH hop through DTN)
    SSHResult run_on_compute(const std::string& node, const std::string& command,
                             int timeout_secs = 30);

    // Get the cluster username
    std::string cluster_user();

    std::unique_ptr<TccpService> service_;
    fs::path project_dir_;
    std::string project_name_;

private:
    static std::atomic<int> counter_;
    void write_project_files();
};

// Global test environment: cleans all tccp-test-* projects before suite runs
class TestEnvironment : public ::testing::Environment {
public:
    void SetUp() override;
    void TearDown() override {}

    // Clean ALL tccp-test-* state files from the default state dir
    static void clean_all_test_projects();
};
