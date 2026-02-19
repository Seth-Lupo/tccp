#include "time_utils.hpp"
#include <fmt/format.h>
#include <ctime>
#include <sstream>
#include <iomanip>

// Cross-platform ISO timestamp parsing (YYYY-MM-DDTHH:MM:SS)
static bool parse_iso(const char* s, struct tm* out) {
    *out = {};
    std::istringstream ss(s);
    ss >> std::get_time(out, "%Y-%m-%dT%H:%M:%S");
    return !ss.fail();
}

std::string format_duration(const std::string& start_time, const std::string& end_time) {
    if (start_time.empty()) return "-";

    struct tm start_tm = {};
    if (!parse_iso(start_time.c_str(), &start_tm)) {
        return "?";
    }
    std::time_t start_t = mktime(&start_tm);

    std::time_t end_t;
    if (!end_time.empty()) {
        struct tm end_tm = {};
        if (!parse_iso(end_time.c_str(), &end_tm)) {
            return "?";
        }
        end_t = mktime(&end_tm);
    } else {
        end_t = std::time(nullptr);
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
    if (!parse_iso(iso_time.c_str(), &tm_buf)) {
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
