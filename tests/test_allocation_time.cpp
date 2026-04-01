#include <gtest/gtest.h>
#include <managers/allocation_manager.hpp>

// ── parse_time_minutes edge cases ──────────────────────────────

TEST(AllocationTime, ZeroTime) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("0:00:00"), 0);
}

TEST(AllocationTime, SubMinuteRoundsUp) {
    // s > 0 adds 1 minute
    EXPECT_EQ(AllocationManager::parse_time_minutes("0:00:30"), 1);
}

TEST(AllocationTime, FiveMinutes) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("0:05:00"), 5);
}

TEST(AllocationTime, FourHours) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("4:00:00"), 240);
}

TEST(AllocationTime, MaxDayTime) {
    // 23:59:59 → 23*60 + 59 + 1 (for 59s) = 1440
    EXPECT_EQ(AllocationManager::parse_time_minutes("23:59:59"), 1440);
}

TEST(AllocationTime, EmptyReturnsDefault) {
    EXPECT_EQ(AllocationManager::parse_time_minutes(""), 240);
}

TEST(AllocationTime, TwoPartTime) {
    // "1:00" → sscanf reads h=1, m=0 → 60
    EXPECT_EQ(AllocationManager::parse_time_minutes("1:00"), 60);
}

TEST(AllocationTime, GarbageReturnsDefault) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("not-a-time"), 240);
}

TEST(AllocationTime, OneSecond) {
    EXPECT_EQ(AllocationManager::parse_time_minutes("0:00:01"), 1);
}

TEST(AllocationTime, LargeHours) {
    // 100:00:00 → 6000
    EXPECT_EQ(AllocationManager::parse_time_minutes("100:00:00"), 6000);
}
