#include "expect.hpp"
#include <libssh2.h>
#include <chrono>
#include <thread>

ExpectMatcher::ExpectMatcher() = default;

void ExpectMatcher::set_io_mutex(std::shared_ptr<std::mutex> mtx) {
    io_mutex_ = std::move(mtx);
}

MatchResult ExpectMatcher::expect(
    LIBSSH2_CHANNEL* channel,
    const std::vector<Pattern>& patterns,
    std::chrono::seconds timeout) {

    if (!channel) {
        return MatchResult{false, 0, "", ""};
    }

    auto start = std::chrono::high_resolution_clock::now();
    MatchResult result{false, 0, "", ""};

    while (true) {
        // Check if we have a match in current buffer
        if (check_patterns(buffer_, patterns, result)) {
            return result;
        }

        // Check timeout
        auto elapsed = std::chrono::high_resolution_clock::now() - start;
        if (elapsed > timeout) {
            result.matched = false;
            result.before_text = buffer_;
            return result;
        }

        // Calculate remaining timeout
        auto remaining = timeout - elapsed;
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);

        // Wait for data with timeout
        if (!wait_for_data(channel, remaining_ms)) {
            result.matched = false;
            result.before_text = buffer_;
            return result;
        }

        // Read available data
        std::string data = read_nonblocking(channel);
        if (data.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        buffer_ += data;
    }
}

std::string ExpectMatcher::read_nonblocking(LIBSSH2_CHANNEL* channel) {
    char buf[4096];
    int n;
    if (io_mutex_) {
        std::lock_guard<std::mutex> lock(*io_mutex_);
        n = libssh2_channel_read(channel, buf, sizeof(buf) - 1);
    } else {
        n = libssh2_channel_read(channel, buf, sizeof(buf) - 1);
    }

    if (n == LIBSSH2_ERROR_EAGAIN || n < 0) {
        return "";
    }

    return std::string(buf, n);
}

bool ExpectMatcher::check_patterns(const std::string& buffer,
                                   const std::vector<Pattern>& patterns,
                                   MatchResult& result) {
    for (size_t i = 0; i < patterns.size(); ++i) {
        std::smatch match;
        if (std::regex_search(buffer, match, patterns[i].regex)) {
            result.matched = true;
            result.pattern_index = i;
            result.matched_text = match[0];
            result.before_text = buffer.substr(0, match.position());
            return true;
        }
    }
    return false;
}

bool ExpectMatcher::wait_for_data(LIBSSH2_CHANNEL* channel, std::chrono::milliseconds timeout) {
    // Since libssh2 handles the socket internally, we can't directly select on it.
    // We rely on non-blocking reads and small sleep intervals.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
}
