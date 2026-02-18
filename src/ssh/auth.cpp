#include "auth.hpp"
#include <platform/platform.hpp>
#include <libssh2.h>

// ── Built-in patterns ────────────────────────────────────────────────

static const std::vector<Challenge> BUILTIN_SHELL_PROMPTS{
    {Pattern("\\[.+@.+ ~\\]\\$ "),  ChallengeType::SHELL_READY},
    {Pattern("\\[.+@.+ .*\\]\\$ "), ChallengeType::SHELL_READY},
    {Pattern("\\$ "),                ChallengeType::SHELL_READY},
    {Pattern("# "),                  ChallengeType::SHELL_READY},
    {Pattern("> "),                  ChallengeType::SHELL_READY},
};

static const std::vector<Challenge> BUILTIN_DUO_PROMPTS{
    {Pattern("Duo"),              ChallengeType::DUO},
    {Pattern("[Oo]ption"),        ChallengeType::DUO},
    {Pattern("[Pp]asscode"),      ChallengeType::DUO},
    {Pattern("[Ff]actor"),        ChallengeType::DUO},
    {Pattern("Press any key"),    ChallengeType::DUO},
};

static const std::vector<Challenge> BUILTIN_PASSWORD_PROMPTS{
    {Pattern("[Pp]assword"),      ChallengeType::PASSWORD},
};

// ── ShellNegotiator ──────────────────────────────────────────────────

ShellNegotiator::ShellNegotiator() = default;

void ShellNegotiator::set_password(const std::string& password) {
    password_ = password;
    handle_password_ = true;
}

void ShellNegotiator::set_duo_response(const std::string& response) {
    duo_response_ = response;
    handle_duo_ = true;
}

void ShellNegotiator::add_challenge(const std::string& pattern, ChallengeType type) {
    extra_challenges_.push_back({Pattern(pattern), type});
}

void ShellNegotiator::set_timeout(std::chrono::seconds timeout) {
    timeout_ = timeout;
}

std::vector<Challenge> ShellNegotiator::build_challenges() const {
    std::vector<Challenge> challenges;

    // Shell prompts always come first — they're the goal
    challenges.insert(challenges.end(),
                      BUILTIN_SHELL_PROMPTS.begin(), BUILTIN_SHELL_PROMPTS.end());

    // Password prompts (only if we have a password to send)
    if (handle_password_) {
        challenges.insert(challenges.end(),
                          BUILTIN_PASSWORD_PROMPTS.begin(), BUILTIN_PASSWORD_PROMPTS.end());
    }

    // Duo prompts (only if we're set up for Duo)
    if (handle_duo_) {
        challenges.insert(challenges.end(),
                          BUILTIN_DUO_PROMPTS.begin(), BUILTIN_DUO_PROMPTS.end());
    }

    // Any custom patterns the caller added
    challenges.insert(challenges.end(),
                      extra_challenges_.begin(), extra_challenges_.end());

    return challenges;
}

bool ShellNegotiator::write_channel(LIBSSH2_CHANNEL* channel, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = libssh2_channel_write(channel,
                                          data.c_str() + written,
                                          data.size() - written);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            platform::sleep_ms(10);
            continue;
        }
        if (n < 0) return false;
        written += n;
    }
    return true;
}

SSHResult ShellNegotiator::negotiate(LIBSSH2_CHANNEL* channel, StatusCallback callback) {
    if (!channel) {
        return SSHResult{-1, "", "No channel"};
    }

    auto challenges = build_challenges();

    // Split into flat pattern/tag vectors for ExpectMatcher
    std::vector<Pattern> patterns;
    std::vector<ChallengeType> tags;
    patterns.reserve(challenges.size());
    tags.reserve(challenges.size());
    for (auto& c : challenges) {
        patterns.push_back(c.pattern);
        tags.push_back(c.type);
    }

    ExpectMatcher matcher;
    auto deadline = std::chrono::steady_clock::now() + timeout_;
    bool password_sent = false;
    bool duo_sent = false;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(deadline - now);
        if (remaining.count() <= 0) break;

        auto result = matcher.expect(channel, patterns, remaining);

        if (!result.matched) {
            // Timeout with no recognized prompt
            std::string buf = matcher.get_buffer();
            if (buf.empty()) {
                return SSHResult{-1, "", "Shell prompt not detected (no output received)"};
            }
            return SSHResult{-1, "", "Shell prompt not detected: " + buf.substr(0, 200)};
        }

        switch (tags[result.pattern_index]) {

        case ChallengeType::SHELL_READY:
            return SSHResult{0, matcher.get_buffer(), ""};

        case ChallengeType::PASSWORD:
            if (password_sent) {
                // Already sent password once — probably wrong password
                return SSHResult{-1, "", "Password rejected (prompted again after sending)"};
            }
            if (callback) callback("Sending password...");
            if (!write_channel(channel, password_ + "\n")) {
                return SSHResult{-1, "", "Failed to send password"};
            }
            password_sent = true;
            matcher.clear_buffer();
            break;

        case ChallengeType::DUO:
            if (duo_sent) {
                // Already sent Duo response — probably failed or timed out
                return SSHResult{-1, "", "Duo authentication failed (prompted again)"};
            }
            if (callback) callback("Duo push sent, check your phone...");
            if (!write_channel(channel, duo_response_ + "\n")) {
                return SSHResult{-1, "", "Failed to send Duo response"};
            }
            duo_sent = true;
            matcher.clear_buffer();
            break;
        }
    }

    return SSHResult{-1, "", "Shell negotiation timed out"};
}

// ── send_command (utility) ───────────────────────────────────────────

SSHResult send_command(LIBSSH2_CHANNEL* channel,
                       const std::string& command,
                       const std::vector<std::string>& expected_prompts) {
    ExpectMatcher matcher;

    int written = libssh2_channel_write(channel, command.c_str(), command.length());
    if (written != static_cast<int>(command.length())) {
        return SSHResult{-1, "", "Failed to send command"};
    }

    std::vector<Pattern> patterns;
    for (const auto& prompt : expected_prompts) {
        patterns.emplace_back(prompt);
    }

    auto result = matcher.expect(channel, patterns, std::chrono::seconds(30));
    if (!result.matched) {
        return SSHResult{-1, "", "No response from server: " + matcher.get_buffer()};
    }

    return SSHResult{0, matcher.get_buffer(), ""};
}
