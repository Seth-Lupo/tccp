#include <gtest/gtest.h>
#include <managers/gpu_discovery.hpp>

static std::vector<GpuResource> make_resources() {
    return {
        {"gpu",      "a100", 4, 2, 4, 524288, 64},
        {"preempt",  "a100", 4, 1, 2, 524288, 64},
        {"gpu",      "v100", 2, 3, 5, 262144, 32},
        {"bigmem",   "a100", 8, 0, 2, 1048576, 128},
    };
}

TEST(GpuSelection, FindExactType) {
    auto resources = make_resources();
    auto result = find_gpu_partition(resources, "a100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.gpu_type, "a100");
    // Should prefer partition with available nodes
    EXPECT_EQ(result.partition, "gpu");
}

TEST(GpuSelection, FindV100) {
    auto resources = make_resources();
    auto result = find_gpu_partition(resources, "v100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.partition, "gpu");
    EXPECT_EQ(result.gpu_type, "v100");
}

TEST(GpuSelection, NotEnoughGPUs) {
    auto resources = make_resources();
    // Need 5 GPUs per node, but max is 8 on bigmem (no avail) or 4 on gpu
    auto result = find_gpu_partition(resources, "a100", 5);

    // Only bigmem has 8, but it has 0 available nodes
    // Still should be found (just lower score)
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.partition, "bigmem");
}

TEST(GpuSelection, NoMatch) {
    auto resources = make_resources();
    auto result = find_gpu_partition(resources, "h100", 1);

    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.error.empty());
}

TEST(GpuSelection, EmptyResources) {
    std::vector<GpuResource> empty;
    auto result = find_gpu_partition(empty, "a100", 1);

    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.error.empty());
}

TEST(GpuSelection, UserPartitionFilter) {
    auto resources = make_resources();
    std::vector<std::string> allowed = {"preempt"};
    auto result = find_gpu_partition(resources, "a100", 1, allowed);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.partition, "preempt");
}

TEST(GpuSelection, UserPartitionFilter_NoMatch) {
    auto resources = make_resources();
    std::vector<std::string> allowed = {"nonexistent"};
    auto result = find_gpu_partition(resources, "a100", 1, allowed);

    EXPECT_FALSE(result.found);
}

TEST(GpuSelection, PrefersFewerGPUsPerNode) {
    // When two partitions are equal in availability, prefer the one that wastes fewer GPUs
    std::vector<GpuResource> resources = {
        {"small_gpu", "a100", 2, 3, 5, 262144, 32},
        {"big_gpu",   "a100", 8, 3, 5, 524288, 64},
    };
    auto result = find_gpu_partition(resources, "a100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.partition, "small_gpu");
}
