#include <gtest/gtest.h>
#include <managers/gpu_discovery.hpp>
#include <core/resource_spec.hpp>

// ── Legacy resources (no variant splitting — raw GRES types) ──

static std::vector<GpuResource> make_resources() {
    return {
        {"gpu",      "a100", 4, 2, 4, 524288, 64, ""},
        {"preempt",  "a100", 4, 1, 2, 524288, 64, ""},
        {"gpu",      "v100", 2, 3, 5, 262144, 32, ""},
        {"bigmem",   "a100", 8, 0, 2, 1048576, 128, ""},
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
        {"small_gpu", "a100", 2, 3, 5, 262144, 32, ""},
        {"big_gpu",   "a100", 8, 3, 5, 524288, 64, ""},
    };
    auto result = find_gpu_partition(resources, "a100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.partition, "small_gpu");
}

// ── Variant-aware tests ────────────────────────────────────

// Simulates what discover_gpu_resources would produce after variant splitting
static std::vector<GpuResource> make_variant_resources() {
    return {
        // partition, gpu_type (variant id), gpu_per_node, avail, total, mem, cpus, node_prefix
        {"gpu", "a100-40gb", 4, 3, 6, 524288,  64, "cc1gpu"},
        {"gpu", "a100-80gb", 4, 2, 4, 524288,  64, "s1cmp"},
        {"gpu", "v100",      2, 3, 5, 262144,  32, ""},
    };
}

TEST(GpuVariant, BareA100PrefersCheapest) {
    // "a100" (bare base type) should match both variants, prefer tier-1 (40GB)
    auto resources = make_variant_resources();
    auto result = find_gpu_partition(resources, "a100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.gpu_type, "a100-40gb");
    EXPECT_EQ(result.node_prefix, "cc1gpu");
}

TEST(GpuVariant, ExplicitA10080gb) {
    // Explicit "a100-80gb" should only match the 80GB variant
    auto resources = make_variant_resources();
    auto result = find_gpu_partition(resources, "a100-80gb", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.gpu_type, "a100-80gb");
    EXPECT_EQ(result.node_prefix, "s1cmp");
}

TEST(GpuVariant, ExplicitA10040gb) {
    auto resources = make_variant_resources();
    auto result = find_gpu_partition(resources, "a100-40gb", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.gpu_type, "a100-40gb");
    EXPECT_EQ(result.node_prefix, "cc1gpu");
}

TEST(GpuVariant, V100Unaffected) {
    // v100 has no variants — should work as before
    auto resources = make_variant_resources();
    auto result = find_gpu_partition(resources, "v100", 1);

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.gpu_type, "v100");
    EXPECT_TRUE(result.node_prefix.empty());
}

TEST(GpuVariant, NoMatchForWrongVariant) {
    // "a100-sxm" is not a known variant
    auto resources = make_variant_resources();
    auto result = find_gpu_partition(resources, "a100-sxm", 1);

    EXPECT_FALSE(result.found);
}

TEST(GpuVariant, FindVariantById) {
    const auto* v = find_variant_by_id("a100-40gb");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->base_type, "a100");
    EXPECT_EQ(v->node_prefix, "cc1gpu");
    EXPECT_EQ(v->mem_gb, 40);
    EXPECT_EQ(v->tier, 1);

    EXPECT_EQ(find_variant_by_id("nonexistent"), nullptr);
}

TEST(GpuVariant, FindVariantsByBase) {
    auto variants = find_variants_by_base("a100");
    EXPECT_EQ(variants.size(), 2u);

    auto none = find_variants_by_base("h100");
    EXPECT_TRUE(none.empty());
}

TEST(GpuVariant, FormatGpuGresStripsVariant) {
    // format_gpu_gres with a variant ID should produce the base GRES type
    std::string gres = format_gpu_gres("a100-40gb", 1);
    EXPECT_EQ(gres, "gpu:a100:1");

    // Raw type passes through unchanged
    std::string gres2 = format_gpu_gres("v100", 2);
    EXPECT_EQ(gres2, "gpu:v100:2");
}
