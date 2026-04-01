#include <gtest/gtest.h>
#include <managers/state_store.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// Offline tests verifying state file edge cases that could cause bad state.

class StateInvariantsTest : public ::testing::Test {
protected:
    void SetUp() override {
        project_name_ = "tccp-test-invariants-" + std::to_string(getpid());
        store_ = std::make_unique<StateStore>(project_name_);
    }

    void TearDown() override {
        if (fs::exists(store_->path()))
            fs::remove(store_->path());
    }

    std::string project_name_;
    std::unique_ptr<StateStore> store_;
};

// ── Concurrent-write simulation: partial YAML ────────────────────
TEST_F(StateInvariantsTest, TruncatedYamlRecovery) {
    // Simulate a crash mid-write: valid YAML prefix, then truncated
    fs::create_directories(store_->path().parent_path());
    {
        std::ofstream f(store_->path().string());
        f << "allocations:\n"
          << "  - slurm_id: \"12345\"\n"
          << "    node: \"node-1\"\n"
          << "    partition: \"batch\"\n"
          // truncated mid-allocation, missing rest of YAML
          ;
    }

    auto state = store_->load();
    // Should either parse what it can or return empty — never crash
    // The truncated YAML is technically valid (missing fields get defaults)
    if (!state.allocations.empty()) {
        EXPECT_EQ(state.allocations[0].slurm_id, "12345");
    }
}

// ── Extra unknown fields don't break loading ─────────────────────
TEST_F(StateInvariantsTest, UnknownFieldsIgnored) {
    fs::create_directories(store_->path().parent_path());
    {
        std::ofstream f(store_->path().string());
        f << "allocations:\n"
          << "  - slurm_id: \"12345\"\n"
          << "    node: \"node-1\"\n"
          << "    future_field: true\n"
          << "    another_unknown: 42\n"
          << "jobs: []\n";
    }

    auto state = store_->load();
    ASSERT_EQ(state.allocations.size(), 1u);
    EXPECT_EQ(state.allocations[0].slurm_id, "12345");
}

// ── Duplicate SLURM IDs in state file ────────────────────────────
TEST_F(StateInvariantsTest, DuplicateAllocIdsInState) {
    ProjectState state;
    AllocationState a1; a1.slurm_id = "12345"; a1.node = "node-1";
    AllocationState a2; a2.slurm_id = "12345"; a2.node = "node-2"; // same ID!
    state.allocations = {a1, a2};
    store_->save(state);

    auto loaded = store_->load();
    // Both should survive serialization (reconcile is what deduplicates)
    EXPECT_EQ(loaded.allocations.size(), 2u);
}

// ── Job references allocation that's not in allocations list ─────
TEST_F(StateInvariantsTest, OrphanedJobReference) {
    ProjectState state;
    JobState j;
    j.job_id = "orphan-job";
    j.job_name = "train";
    j.alloc_slurm_id = "99999";  // no allocation with this ID
    j.completed = false;
    j.init_complete = true;
    state.jobs.push_back(j);
    // No allocations at all

    store_->save(state);
    auto loaded = store_->load();

    // State file preserves the orphan (reconcile cleans it)
    ASSERT_EQ(loaded.jobs.size(), 1u);
    EXPECT_EQ(loaded.jobs[0].alloc_slurm_id, "99999");
}

// ── Very large state file (many allocations/jobs) ────────────────
TEST_F(StateInvariantsTest, LargeStateRoundTrip) {
    ProjectState state;
    for (int i = 0; i < 100; i++) {
        AllocationState a;
        a.slurm_id = std::to_string(10000 + i);
        a.node = "node-" + std::to_string(i % 10);
        a.partition = "batch";
        a.duration_minutes = 240;
        state.allocations.push_back(a);
    }
    for (int i = 0; i < 200; i++) {
        JobState j;
        j.job_id = "job-" + std::to_string(i);
        j.job_name = (i % 2 == 0) ? "train" : "eval";
        j.alloc_slurm_id = std::to_string(10000 + (i % 100));
        j.completed = (i < 150);
        j.exit_code = (i < 150) ? 0 : -1;
        state.jobs.push_back(j);
    }

    store_->save(state);
    auto loaded = store_->load();

    EXPECT_EQ(loaded.allocations.size(), 100u);
    EXPECT_EQ(loaded.jobs.size(), 200u);
}

// ── Allocation with negative/zero duration ───────────────────────
TEST_F(StateInvariantsTest, ZeroDurationAllocation) {
    ProjectState state;
    AllocationState a;
    a.slurm_id = "12345";
    a.duration_minutes = 0;
    state.allocations.push_back(a);

    store_->save(state);
    auto loaded = store_->load();

    ASSERT_EQ(loaded.allocations.size(), 1u);
    EXPECT_EQ(loaded.allocations[0].duration_minutes, 0);
}

// ── Job with exit_code = -1 (unknown) round trips ────────────────
TEST_F(StateInvariantsTest, NegativeExitCodePreserved) {
    ProjectState state;
    JobState j;
    j.job_id = "crash-job";
    j.exit_code = -1;
    j.completed = true;
    state.jobs.push_back(j);

    store_->save(state);
    auto loaded = store_->load();

    ASSERT_EQ(loaded.jobs.size(), 1u);
    EXPECT_EQ(loaded.jobs[0].exit_code, -1);
}

// ── Save over existing state is atomic (no append) ───────────────
TEST_F(StateInvariantsTest, SaveReplacesEntireFile) {
    // Save state with 5 allocations
    ProjectState big;
    for (int i = 0; i < 5; i++) {
        AllocationState a; a.slurm_id = std::to_string(i);
        big.allocations.push_back(a);
    }
    store_->save(big);

    // Save state with 1 allocation
    ProjectState small;
    AllocationState a; a.slurm_id = "only-one";
    small.allocations.push_back(a);
    store_->save(small);

    auto loaded = store_->load();
    EXPECT_EQ(loaded.allocations.size(), 1u);
    EXPECT_EQ(loaded.allocations[0].slurm_id, "only-one");
}
