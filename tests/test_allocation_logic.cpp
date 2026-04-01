#include <gtest/gtest.h>
#include <managers/state_store.hpp>
#include <core/resource_spec.hpp>
#include <managers/allocation_manager.hpp>

// ── parse_time_minutes ─────────────────────────────────────────

TEST(AllocationLogic, ParseTime_HHMMSS) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("4:00:00"), 240);
    EXPECT_EQ(AllocationManager::parse_time_minutes("0:30:00"), 30);
    EXPECT_EQ(AllocationManager::parse_time_minutes("1:30:00"), 90);
}

TEST(AllocationLogic, ParseTime_MMSS) {
    // MM:SS is interpreted as HH:MM by the parser (H*60 + M)
    EXPECT_EQ(AllocationManager::parse_time_minutes("30:00"), 1800);
    EXPECT_EQ(AllocationManager::parse_time_minutes("5:00"), 300);
}

TEST(AllocationLogic, ParseTime_EmptyAndInvalid) {
    // Returns default (240) for unparseable input
    EXPECT_EQ(AllocationManager::parse_time_minutes(""), 240);
    EXPECT_EQ(AllocationManager::parse_time_minutes("garbage"), 240);
}

// ── Resource compatibility (allocation vs job) ─────────────────

static SlurmDefaults make_slurm(const std::string& partition, int cpus,
                                  const std::string& memory, const std::string& gpu_type,
                                  int gpu_count) {
    SlurmDefaults d{};
    d.partition = partition;
    d.cpus_per_task = cpus;
    d.memory = memory;
    d.gpu_type = gpu_type;
    d.gpu_count = gpu_count;
    d.nodes = 1;
    return d;
}

TEST(AllocationLogic, ResourceCompat_CPUOnlyMatch) {
    auto alloc = make_slurm("batch", 4, "8G", "", 0);
    auto job = make_slurm("batch", 2, "4G", "", 0);
    EXPECT_TRUE(resources_compatible(alloc, job));
}

TEST(AllocationLogic, ResourceCompat_ExactCPUOnly) {
    auto alloc = make_slurm("batch", 1, "1G", "", 0);
    auto job = make_slurm("batch", 1, "1G", "", 0);
    EXPECT_TRUE(resources_compatible(alloc, job));
}

TEST(AllocationLogic, ResourceCompat_InsufficientCPU) {
    auto alloc = make_slurm("batch", 1, "8G", "", 0);
    auto job = make_slurm("batch", 4, "4G", "", 0);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(AllocationLogic, ResourceCompat_PartitionMismatch) {
    auto alloc = make_slurm("gpu", 4, "16G", "a100", 1);
    auto job = make_slurm("batch", 4, "16G", "a100", 1);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(AllocationLogic, ResourceCompat_EmptyPartitionMatchesAny) {
    auto alloc = make_slurm("batch", 1, "1G", "", 0);
    auto job = make_slurm("", 1, "1G", "", 0);
    EXPECT_TRUE(resources_compatible(alloc, job));
}
