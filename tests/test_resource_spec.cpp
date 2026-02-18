#include <gtest/gtest.h>
#include <core/resource_spec.hpp>

// ── parse_memory_mb ─────────────────────────────────────────

TEST(ResourceSpec, ParseMemoryMB_Empty) {
    EXPECT_EQ(parse_memory_mb(""), 0);
}

TEST(ResourceSpec, ParseMemoryMB_Megabytes) {
    EXPECT_EQ(parse_memory_mb("4096M"), 4096);
    EXPECT_EQ(parse_memory_mb("4096MB"), 4096);
    EXPECT_EQ(parse_memory_mb("4096"), 4096);  // default to MB
}

TEST(ResourceSpec, ParseMemoryMB_Gigabytes) {
    EXPECT_EQ(parse_memory_mb("4G"), 4096);
    EXPECT_EQ(parse_memory_mb("4GB"), 4096);
    EXPECT_EQ(parse_memory_mb("128G"), 131072);
}

TEST(ResourceSpec, ParseMemoryMB_Terabytes) {
    EXPECT_EQ(parse_memory_mb("1T"), 1048576);
    EXPECT_EQ(parse_memory_mb("1TB"), 1048576);
}

TEST(ResourceSpec, ParseMemoryMB_CaseInsensitive) {
    EXPECT_EQ(parse_memory_mb("4g"), 4096);
    EXPECT_EQ(parse_memory_mb("4gb"), 4096);
    EXPECT_EQ(parse_memory_mb("1024m"), 1024);
}

// ── resources_compatible ────────────────────────────────────

static SlurmDefaults make_defaults(const std::string& partition, int cpus,
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

TEST(ResourceSpec, Compatible_ExactMatch) {
    auto alloc = make_defaults("gpu", 4, "16G", "a100", 1);
    auto job = make_defaults("gpu", 4, "16G", "a100", 1);
    EXPECT_TRUE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Compatible_AllocHasMore) {
    auto alloc = make_defaults("gpu", 8, "32G", "a100", 2);
    auto job = make_defaults("gpu", 4, "16G", "a100", 1);
    EXPECT_TRUE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Incompatible_DifferentPartition) {
    auto alloc = make_defaults("gpu", 4, "16G", "a100", 1);
    auto job = make_defaults("preempt", 4, "16G", "a100", 1);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Incompatible_NotEnoughCPUs) {
    auto alloc = make_defaults("gpu", 2, "16G", "a100", 1);
    auto job = make_defaults("gpu", 4, "16G", "a100", 1);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Incompatible_NotEnoughMemory) {
    auto alloc = make_defaults("gpu", 4, "8G", "a100", 1);
    auto job = make_defaults("gpu", 4, "16G", "a100", 1);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Incompatible_DifferentGPUType) {
    auto alloc = make_defaults("gpu", 4, "16G", "v100", 1);
    auto job = make_defaults("gpu", 4, "16G", "a100", 1);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Incompatible_NotEnoughGPUs) {
    auto alloc = make_defaults("gpu", 4, "16G", "a100", 1);
    auto job = make_defaults("gpu", 4, "16G", "a100", 2);
    EXPECT_FALSE(resources_compatible(alloc, job));
}

TEST(ResourceSpec, Compatible_DefaultPartition) {
    // Empty partition on job = any partition matches
    auto alloc = make_defaults("batch", 4, "16G", "a100", 1);
    auto job = make_defaults("", 4, "16G", "a100", 1);
    EXPECT_TRUE(resources_compatible(alloc, job));

    // Also works with preempt allocation
    auto alloc2 = make_defaults("preempt", 4, "16G", "a100", 1);
    EXPECT_TRUE(resources_compatible(alloc2, job));
}

TEST(ResourceSpec, Compatible_NoGPURequirement) {
    auto alloc = make_defaults("batch", 4, "16G", "a100", 2);
    auto job = make_defaults("batch", 4, "16G", "", 0);
    EXPECT_TRUE(resources_compatible(alloc, job));
}

// ── format_gpu_gres ─────────────────────────────────────────

TEST(ResourceSpec, FormatGpuGres_Normal) {
    EXPECT_EQ(format_gpu_gres("a100", 4), "gpu:a100:4");
}

TEST(ResourceSpec, FormatGpuGres_NoGPU) {
    EXPECT_EQ(format_gpu_gres("", 0), "");
    EXPECT_EQ(format_gpu_gres("a100", 0), "");
    EXPECT_EQ(format_gpu_gres("", 1), "");
}
