#include <gtest/gtest.h>
#include <core/time_utils.hpp>

TEST(TimeUtils, FormatDurationEmpty) {
    EXPECT_EQ(format_duration(""), "-");
}

TEST(TimeUtils, FormatDurationBadParse) {
    EXPECT_EQ(format_duration("not-a-date"), "?");
}

TEST(TimeUtils, FormatDurationSeconds) {
    // 45 seconds apart
    EXPECT_EQ(format_duration("2025-01-15T10:00:00", "2025-01-15T10:00:45"), "45s");
}

TEST(TimeUtils, FormatDurationMinutes) {
    // 5 minutes 30 seconds apart
    EXPECT_EQ(format_duration("2025-01-15T10:00:00", "2025-01-15T10:05:30"), "5m30s");
}

TEST(TimeUtils, FormatDurationHours) {
    // 2 hours 15 minutes apart
    EXPECT_EQ(format_duration("2025-01-15T10:00:00", "2025-01-15T12:15:00"), "2h15m");
}

TEST(TimeUtils, FormatDurationZero) {
    EXPECT_EQ(format_duration("2025-01-15T10:00:00", "2025-01-15T10:00:00"), "0s");
}

TEST(TimeUtils, FormatTimestampEmpty) {
    EXPECT_EQ(format_timestamp(""), "-");
}

TEST(TimeUtils, FormatTimestampBadParse) {
    EXPECT_EQ(format_timestamp("garbage"), "?");
}

TEST(TimeUtils, FormatTimestampNormal) {
    EXPECT_EQ(format_timestamp("2025-01-15T14:35:22"), "2:35pm");
}

TEST(TimeUtils, FormatTimestampMidnight) {
    EXPECT_EQ(format_timestamp("2025-01-15T00:00:00"), "12:00am");
}
