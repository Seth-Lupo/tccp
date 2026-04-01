#include <gtest/gtest.h>
#include <managers/state_store.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class StateStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a unique temp dir to avoid polluting ~/.tccp/state/
        project_name_ = "tccp-test-statestore-" + std::to_string(getpid());
        store_ = std::make_unique<StateStore>(project_name_);
    }

    void TearDown() override {
        // Clean up the state file
        if (fs::exists(store_->path())) {
            fs::remove(store_->path());
        }
    }

    std::string project_name_;
    std::unique_ptr<StateStore> store_;
};

TEST_F(StateStoreTest, LoadNonExistentReturnsEmpty) {
    auto state = store_->load();
    EXPECT_TRUE(state.allocations.empty());
    EXPECT_TRUE(state.jobs.empty());
    EXPECT_TRUE(state.last_sync_node.empty());
    EXPECT_TRUE(state.last_sync_scratch.empty());
    EXPECT_TRUE(state.last_sync_manifest.empty());
}

TEST_F(StateStoreTest, SaveAndLoadEmptyState) {
    ProjectState state;
    store_->save(state);
    EXPECT_TRUE(fs::exists(store_->path()));

    auto loaded = store_->load();
    EXPECT_TRUE(loaded.allocations.empty());
    EXPECT_TRUE(loaded.jobs.empty());
}

TEST_F(StateStoreTest, AllocationRoundTrip) {
    ProjectState state;
    AllocationState a;
    a.slurm_id = "12345";
    a.node = "compute-01";
    a.start_time = "2025-01-15T10:30:00";
    a.duration_minutes = 120;
    a.active_job_id = "job-abc";
    a.partition = "gpu";
    a.nodes = 2;
    a.cpus = 8;
    a.memory = "32G";
    a.gpu_type = "a100";
    a.gpu_count = 4;
    state.allocations.push_back(a);

    store_->save(state);
    auto loaded = store_->load();

    ASSERT_EQ(loaded.allocations.size(), 1u);
    const auto& la = loaded.allocations[0];
    EXPECT_EQ(la.slurm_id, "12345");
    EXPECT_EQ(la.node, "compute-01");
    EXPECT_EQ(la.start_time, "2025-01-15T10:30:00");
    EXPECT_EQ(la.duration_minutes, 120);
    EXPECT_EQ(la.active_job_id, "job-abc");
    EXPECT_EQ(la.partition, "gpu");
    EXPECT_EQ(la.nodes, 2);
    EXPECT_EQ(la.cpus, 8);
    EXPECT_EQ(la.memory, "32G");
    EXPECT_EQ(la.gpu_type, "a100");
    EXPECT_EQ(la.gpu_count, 4);
}

TEST_F(StateStoreTest, JobRoundTrip) {
    ProjectState state;
    JobState j;
    j.job_id = "2025-01-15T10-30-00-000__train";
    j.job_name = "train";
    j.alloc_slurm_id = "12345";
    j.compute_node = "compute-01";
    j.completed = true;
    j.canceled = false;
    j.exit_code = 0;
    j.output_file = "/home/user/output.tar.gz";
    j.scratch_path = "/tmp/user/proj/job1";
    j.partition = "gpu";
    j.init_complete = true;
    j.init_error = "";
    j.output_returned = true;
    j.forwarded_ports = {6006, 8888};
    j.submit_time = "2025-01-15T10:30:00";
    j.start_time = "2025-01-15T10:31:00";
    j.end_time = "2025-01-15T11:00:00";
    state.jobs.push_back(j);

    store_->save(state);
    auto loaded = store_->load();

    ASSERT_EQ(loaded.jobs.size(), 1u);
    const auto& lj = loaded.jobs[0];
    EXPECT_EQ(lj.job_id, "2025-01-15T10-30-00-000__train");
    EXPECT_EQ(lj.job_name, "train");
    EXPECT_EQ(lj.alloc_slurm_id, "12345");
    EXPECT_EQ(lj.compute_node, "compute-01");
    EXPECT_TRUE(lj.completed);
    EXPECT_FALSE(lj.canceled);
    EXPECT_EQ(lj.exit_code, 0);
    EXPECT_EQ(lj.output_file, "/home/user/output.tar.gz");
    EXPECT_EQ(lj.scratch_path, "/tmp/user/proj/job1");
    EXPECT_EQ(lj.partition, "gpu");
    EXPECT_TRUE(lj.init_complete);
    EXPECT_TRUE(lj.init_error.empty());
    EXPECT_TRUE(lj.output_returned);
    ASSERT_EQ(lj.forwarded_ports.size(), 2u);
    EXPECT_EQ(lj.forwarded_ports[0], 6006);
    EXPECT_EQ(lj.forwarded_ports[1], 8888);
    EXPECT_EQ(lj.submit_time, "2025-01-15T10:30:00");
    EXPECT_EQ(lj.start_time, "2025-01-15T10:31:00");
    EXPECT_EQ(lj.end_time, "2025-01-15T11:00:00");
}

TEST_F(StateStoreTest, SyncManifestRoundTrip) {
    ProjectState state;
    state.last_sync_node = "compute-02";
    state.last_sync_scratch = "/tmp/user/proj/job2";

    SyncManifestEntry e;
    e.path = "src/main.py";
    e.mtime = 1705312200;
    e.size = 4096;
    e.md5 = "abc123def456";
    state.last_sync_manifest.push_back(e);

    store_->save(state);
    auto loaded = store_->load();

    EXPECT_EQ(loaded.last_sync_node, "compute-02");
    EXPECT_EQ(loaded.last_sync_scratch, "/tmp/user/proj/job2");
    ASSERT_EQ(loaded.last_sync_manifest.size(), 1u);
    EXPECT_EQ(loaded.last_sync_manifest[0].path, "src/main.py");
    EXPECT_EQ(loaded.last_sync_manifest[0].mtime, 1705312200);
    EXPECT_EQ(loaded.last_sync_manifest[0].size, 4096);
    EXPECT_EQ(loaded.last_sync_manifest[0].md5, "abc123def456");
}

TEST_F(StateStoreTest, MultipleAllocationsAndJobs) {
    ProjectState state;

    for (int i = 0; i < 3; i++) {
        AllocationState a;
        a.slurm_id = std::to_string(10000 + i);
        a.node = "node-" + std::to_string(i);
        a.partition = "batch";
        state.allocations.push_back(a);
    }

    for (int i = 0; i < 5; i++) {
        JobState j;
        j.job_id = "job-" + std::to_string(i);
        j.job_name = "train";
        j.completed = (i < 3);
        state.jobs.push_back(j);
    }

    store_->save(state);
    auto loaded = store_->load();

    EXPECT_EQ(loaded.allocations.size(), 3u);
    EXPECT_EQ(loaded.jobs.size(), 5u);
}

TEST_F(StateStoreTest, CorruptYamlReturnsFreshState) {
    // Write garbage to the state file
    fs::create_directories(store_->path().parent_path());
    {
        std::ofstream f(store_->path().string());
        f << "{{{{not valid yaml at all::::";
    }

    auto loaded = store_->load();
    EXPECT_TRUE(loaded.allocations.empty());
    EXPECT_TRUE(loaded.jobs.empty());
}

TEST_F(StateStoreTest, EmptyFieldsHaveDefaults) {
    // Write minimal YAML with just an allocation missing most fields
    fs::create_directories(store_->path().parent_path());
    {
        std::ofstream f(store_->path().string());
        f << "allocations:\n  - slurm_id: \"99999\"\njobs: []\n";
    }

    auto loaded = store_->load();
    ASSERT_EQ(loaded.allocations.size(), 1u);
    const auto& a = loaded.allocations[0];
    EXPECT_EQ(a.slurm_id, "99999");
    EXPECT_TRUE(a.node.empty());
    EXPECT_EQ(a.duration_minutes, 240);  // default
    EXPECT_TRUE(a.active_job_id.empty());
    EXPECT_EQ(a.nodes, 1);     // default
    EXPECT_EQ(a.cpus, 1);      // default
    EXPECT_EQ(a.memory, "4G"); // default
    EXPECT_EQ(a.gpu_count, 0); // default
}

TEST_F(StateStoreTest, OverwriteExistingState) {
    // Save state with 2 allocations
    ProjectState state1;
    AllocationState a1; a1.slurm_id = "111";
    AllocationState a2; a2.slurm_id = "222";
    state1.allocations = {a1, a2};
    store_->save(state1);

    // Save state with 1 allocation — should overwrite
    ProjectState state2;
    AllocationState a3; a3.slurm_id = "333";
    state2.allocations = {a3};
    store_->save(state2);

    auto loaded = store_->load();
    ASSERT_EQ(loaded.allocations.size(), 1u);
    EXPECT_EQ(loaded.allocations[0].slurm_id, "333");
}

TEST_F(StateStoreTest, JobWithNoForwardedPorts) {
    ProjectState state;
    JobState j;
    j.job_id = "test-job";
    j.job_name = "train";
    // forwarded_ports left empty
    state.jobs.push_back(j);

    store_->save(state);
    auto loaded = store_->load();

    ASSERT_EQ(loaded.jobs.size(), 1u);
    EXPECT_TRUE(loaded.jobs[0].forwarded_ports.empty());
}

TEST_F(StateStoreTest, StaleEntriesSurviveSerialization) {
    // Simulate stale state: allocation references a dead SLURM job
    ProjectState state;
    AllocationState a;
    a.slurm_id = "99999";  // doesn't exist in SLURM
    a.node = "dead-node";
    a.active_job_id = "nonexistent-job";
    state.allocations.push_back(a);

    JobState j;
    j.job_id = "nonexistent-job";
    j.alloc_slurm_id = "99999";
    j.completed = false;  // still "running" but stale
    state.jobs.push_back(j);

    store_->save(state);
    auto loaded = store_->load();

    // Stale entries must survive serialization — reconcile is the one that prunes them
    ASSERT_EQ(loaded.allocations.size(), 1u);
    EXPECT_EQ(loaded.allocations[0].slurm_id, "99999");
    EXPECT_EQ(loaded.allocations[0].active_job_id, "nonexistent-job");
    ASSERT_EQ(loaded.jobs.size(), 1u);
    EXPECT_EQ(loaded.jobs[0].job_id, "nonexistent-job");
    EXPECT_FALSE(loaded.jobs[0].completed);
}
