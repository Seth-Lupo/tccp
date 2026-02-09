#include "auth.hpp"
#include <libssh2.h>
#include <fmt/format.h>
#include <chrono>

// Common shell prompts to detect authenticated state
static const std::vector<Pattern> SHELL_PROMPTS{
    Pattern("vm-hw[0-9]+\\$ "),           // Tufts homework server
    Pattern("\\[.+@.+ ~\\]\\$ "),         // Standard Linux [user@host ~]$
    Pattern("\\[.+@.+ .*\\]\\$ "),        // [user@host path]$
    Pattern("\\$ "),                       // Simple $
    Pattern("# "),                         // Root prompt
    Pattern("> "),                         // Generic prompt
};

SSHResult send_command(LIBSSH2_CHANNEL* channel,
                       const std::string& command,
                       const std::vector<std::string>& expected_prompts) {
    ExpectMatcher matcher;

    // Send command
    int written = libssh2_channel_write(channel, command.c_str(), command.length());
    if (written != command.length()) {
        return SSHResult{-1, "", "Failed to send command"};
    }

    // Convert expected prompts to Pattern objects
    std::vector<Pattern> patterns;
    for (const auto& prompt : expected_prompts) {
        patterns.emplace_back(prompt);
    }

    // Wait for response
    auto result = matcher.expect(channel, patterns, std::chrono::seconds(30));
    if (!result.matched) {
        return SSHResult{-1, "", "No response from server: " + matcher.get_buffer()};
    }

    return SSHResult{0, matcher.get_buffer(), ""};
}

SSHResult PasswordAuthenticator::authenticate(AuthContext& ctx) {
    if (!ctx.channel) {
        return SSHResult{-1, "", "No channel provided"};
    }

    if (ctx.status) {
        ctx.status("[auth] Authenticating with password...");
    }

    // Send password
    std::string passwd_line = ctx.password + "\n";
    int written = libssh2_channel_write(ctx.channel, passwd_line.c_str(), passwd_line.length());
    if (written != passwd_line.length()) {
        return SSHResult{-1, "", "Failed to send password"};
    }

    // Wait for shell prompt or error
    matcher_.clear_buffer();
    auto result = matcher_.expect(ctx.channel, SHELL_PROMPTS, std::chrono::seconds(30));

    if (!result.matched) {
        std::string output = matcher_.get_buffer();
        if (output.find("Permission denied") != std::string::npos ||
            output.find("Authentication failed") != std::string::npos) {
            return SSHResult{-1, "", "Authentication failed: incorrect password"};
        }
        return SSHResult{-1, "", "Failed to authenticate: " + output};
    }

    if (ctx.status) {
        ctx.status("[auth] Password authentication successful");
    }

    return SSHResult{0, matcher_.get_buffer(), ""};
}

SSHResult KeyAuthenticator::authenticate(AuthContext& ctx) {
    if (!ctx.channel) {
        return SSHResult{-1, "", "No channel provided"};
    }

    if (ctx.status) {
        ctx.status("[auth] Authenticating with SSH key...");
    }

    // libssh2 key auth is typically handled at session level
    // This is a placeholder for when key auth is needed at channel level
    matcher_.clear_buffer();
    auto result = matcher_.expect(ctx.channel, SHELL_PROMPTS, std::chrono::seconds(30));

    if (!result.matched) {
        return SSHResult{-1, "", "Key authentication failed"};
    }

    if (ctx.status) {
        ctx.status("[auth] Key authentication successful");
    }

    return SSHResult{0, matcher_.get_buffer(), ""};
}

SSHResult DuoAuthenticator::authenticate(AuthContext& ctx) {
    if (!ctx.channel) {
        return SSHResult{-1, "", "No channel provided"};
    }

    if (ctx.status) {
        ctx.status("[auth] Authenticating with password + Duo 2FA...");
    }

    // First, send password
    std::string passwd_line = ctx.password + "\n";
    int written = libssh2_channel_write(ctx.channel, passwd_line.c_str(), passwd_line.length());
    if (written != passwd_line.length()) {
        return SSHResult{-1, "", "Failed to send password"};
    }

    // Wait for Duo prompt
    matcher_.clear_buffer();
    std::vector<Pattern> duo_patterns{
        Pattern("Duo two-factor login"),
        Pattern("Passcode or option"),
    };

    auto duo_result = matcher_.expect(ctx.channel, duo_patterns, std::chrono::seconds(10));
    if (!duo_result.matched) {
        // Maybe authenticated without Duo, check for shell prompt
        std::vector<Pattern> shell_patterns = SHELL_PROMPTS;
        duo_result = matcher_.expect(ctx.channel, shell_patterns, std::chrono::seconds(10));
        if (duo_result.matched) {
            if (ctx.status) {
                ctx.status("[auth] Password accepted, no 2FA required");
            }
            return SSHResult{0, matcher_.get_buffer(), ""};
        }
        return SSHResult{-1, "", "Duo authentication not available"};
    }

    if (ctx.status) {
        ctx.status("[auth] Duo prompt detected, sending push...");
    }

    // Send "1" for push notification or handle manual passcode
    std::string duo_response = auto_duo_ ? "1\n" : "push\n";
    written = libssh2_channel_write(ctx.channel, duo_response.c_str(), duo_response.length());
    if (written != duo_response.length()) {
        return SSHResult{-1, "", "Failed to send Duo response"};
    }

    // Wait for authentication to complete
    matcher_.clear_buffer();
    auto final_result = matcher_.expect(ctx.channel, SHELL_PROMPTS, std::chrono::seconds(60));

    if (!final_result.matched) {
        return SSHResult{-1, "", "Duo authentication failed: " + matcher_.get_buffer()};
    }

    if (ctx.status) {
        ctx.status("[auth] Duo authentication successful");
    }

    return SSHResult{0, matcher_.get_buffer(), ""};
}
