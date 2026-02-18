#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <core/types.hpp>
#include "expect.hpp"

// libssh2 forward declaration
typedef struct _LIBSSH2_CHANNEL LIBSSH2_CHANNEL;

// What kind of challenge did we see?
enum class ChallengeType {
    SHELL_READY,   // A shell prompt — negotiation is done
    PASSWORD,      // Server wants a password
    DUO,           // Duo 2FA challenge
};

// A pattern tagged with its meaning
struct Challenge {
    Pattern pattern;
    ChallengeType type;
};

// Reactive shell negotiator — handles any combination/ordering of
// password prompts, Duo 2FA, and shell prompts after SSH transport auth.
//
// Usage:
//   ShellNegotiator neg;
//   neg.set_password(pw);        // optional — only if password prompts are possible
//   neg.set_duo_response("1");   // optional — only if Duo is possible
//   auto result = neg.negotiate(channel, callback);
//
class ShellNegotiator {
public:
    ShellNegotiator();

    // Configure what the negotiator can respond to.
    // Only call these for challenges you expect might appear.
    void set_password(const std::string& password);
    void set_duo_response(const std::string& response = "1");

    // Add a custom challenge pattern (for unusual server prompts)
    void add_challenge(const std::string& pattern, ChallengeType type);

    // Set the overall timeout (default 90s to accommodate Duo push wait)
    void set_timeout(std::chrono::seconds timeout);

    // Run the negotiation loop. Reads from channel, reacts to whatever
    // the server sends, in any order, until a shell prompt appears or timeout.
    SSHResult negotiate(LIBSSH2_CHANNEL* channel, StatusCallback callback = nullptr);

private:
    std::string password_;
    std::string duo_response_;
    bool handle_password_ = false;
    bool handle_duo_ = false;
    std::chrono::seconds timeout_{90};
    std::vector<Challenge> extra_challenges_;

    // Build the full challenge list (builtins + extras)
    std::vector<Challenge> build_challenges() const;

    // Write a string to the channel, handling EAGAIN
    static bool write_channel(LIBSSH2_CHANNEL* channel, const std::string& data);
};

// Helper to send a command and wait for a response
SSHResult send_command(LIBSSH2_CHANNEL* channel,
                       const std::string& command,
                       const std::vector<std::string>& expected_prompts);
