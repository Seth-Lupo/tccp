#pragma once

#include <string>
#include <vector>
#include <regex>
#include <chrono>
#include <memory>
#include <mutex>

// libssh2 forward declaration
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

struct Pattern {
    std::regex regex;
    std::string raw;

    Pattern(const std::string& pattern) : raw(pattern) {
        try {
            regex = std::regex(pattern, std::regex::extended);
        } catch (...) {
            regex = std::regex(std::regex_replace(pattern, std::regex("\\$"), "\\$"));
        }
    }
};

struct MatchResult {
    bool matched;
    size_t pattern_index;
    std::string matched_text;
    std::string before_text;
};

class ExpectMatcher {
public:
    ExpectMatcher();

    // Set io_mutex for thread-safe libssh2 access. Optional — if not set,
    // calls are unprotected (safe only during single-threaded init).
    void set_io_mutex(std::shared_ptr<std::mutex> mtx);

    MatchResult expect(
        LIBSSH2_CHANNEL* channel,
        const std::vector<Pattern>& patterns,
        std::chrono::seconds timeout
    );

    void clear_buffer() { buffer_.clear(); }
    const std::string& get_buffer() const { return buffer_; }

private:
    std::string buffer_;
    std::shared_ptr<std::mutex> io_mutex_;

    std::string read_nonblocking(LIBSSH2_CHANNEL* channel);
    bool check_patterns(const std::string& buffer,
                       const std::vector<Pattern>& patterns,
                       MatchResult& result);
    bool wait_for_data(LIBSSH2_CHANNEL* channel, std::chrono::milliseconds timeout);
};
