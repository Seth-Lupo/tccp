#include "time_utils.hpp"
#include <fmt/format.h>
#include <ctime>

std::string format_duration(const std::string& start_time, const std::string& end_time) {
    if (start_time.empty()) return "-";

    // Parse ISO timestamp: YYYY-MM-DDTHH:MM:SS
    struct tm start_tm = {};
    if (strptime(start_time.c_str(), "%Y-%m-%dT%H:%M:%S", &start_tm) == nullptr) {
        return "?";
    }
    std::time_t start_t = mktime(&start_tm);

    std::time_t end_t;
    if (!end_time.empty()) {
        struct tm end_tm = {};
        if (strptime(end_time.c_str(), "%Y-%m-%dT%H:%M:%S", &end_tm) == nullptr) {
            return "?";
        }
        end_t = mktime(&end_tm);
    } else {
        end_t = std::time(nullptr);  // Still running
    }

    int seconds = static_cast<int>(std::difftime(end_t, start_t));
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;

    if (hours > 0) {
        return fmt::format("{}h{}m", hours, mins);
    } else if (mins > 0) {
        return fmt::format("{}m{}s", mins, secs);
    } else {
        return fmt::format("{}s", secs);
    }
}

std::string format_timestamp(const std::string& iso_time) {
    if (iso_time.empty()) return "-";

    // Parse ISO timestamp: YYYY-MM-DDTHH:MM:SS
    struct tm tm_buf = {};
    if (strptime(iso_time.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_buf) == nullptr) {
        return "?";
    }

    // Format as "8:13pm" (12-hour with am/pm)
    char buf[16];
    std::strftime(buf, sizeof(buf), "%I:%M%p", &tm_buf);
    // Strip leading zero and lowercase am/pm: "08:13PM" → "8:13pm"
    std::string result(buf);
    if (!result.empty() && result[0] == '0') result.erase(0, 1);
    for (auto& c : result) c = std::tolower(c);
    return result;
}
