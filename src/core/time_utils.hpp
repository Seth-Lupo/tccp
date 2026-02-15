#pragma once

#include <string>

// Format the duration between two ISO timestamps (YYYY-MM-DDTHH:MM:SS).
// If end_time is empty, uses current time (for "still running" durations).
// Returns human-readable string like "2h35m", "14m22s", "8s", or "-" if start is empty.
std::string format_duration(const std::string& start_time, const std::string& end_time = "");

// Format an ISO timestamp (YYYY-MM-DDTHH:MM:SS) to "HH:MM" display.
// Returns "-" if empty, "?" on parse failure.
std::string format_timestamp(const std::string& iso_time);
