#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <deque>

class ConnectionFactory;
class SessionMultiplexer;

// MultiplexedShell: interactive shell on a dedicated multiplexed channel.
//
// Unlike ShellRelay, this does NOT block the master channel — programmatic
// commands (dtn_.run(), login_.run()) continue freely in parallel.
//
// Simple usage:
//     MultiplexedShell shell(factory);
//     shell.run("ssh -t compute 'bash'");
//
// Advanced usage (custom relay loop):
//     MultiplexedShell shell(factory);
//     if (!shell.start("ssh -t compute '...'")) return;
//     while (!shell.done()) {
//         std::string text = shell.read_output();
//         // process output...
//     }
//     shell.stop();

class MultiplexedShell {
public:
    explicit MultiplexedShell(ConnectionFactory& factory);
    ~MultiplexedShell();

    // ── Simple interface ────────────────────────────────────────

    // Run command with raw stdin/stdout relay. Blocks until done.
    void run(const std::string& command);

    // ── Advanced lifecycle ──────────────────────────────────────

    // Open a dedicated channel, send command with done marker.
    // Returns false if multiplexer unavailable.
    bool start(const std::string& command);

    // Feed raw channel output through the protocol processor.
    // Skips echoed command, detects done marker, buffers partial markers.
    // Returns text safe to display. Check done() afterward.
    std::string feed(const char* data, int len);

    // Read available output from the channel's output queue.
    // Returns empty string if no output available.
    std::string read_output();

    // True once the done marker has been detected.
    bool done() const;

    // Clean up. Closes the multiplexed channel. Safe to call multiple times.
    void stop();

    // ── Access ──────────────────────────────────────────────────

    int channel_id() const;
    SessionMultiplexer& multiplexer() const;
    int socket() const;

private:
    ConnectionFactory& factory_;
    SessionMultiplexer* mux_ = nullptr;
    int channel_id_ = -1;
    bool started_ = false;
    bool skip_echo_ = true;
    bool done_ = false;
    std::string buf_;

    // Output queue: filled by the multiplexer's output callback
    std::mutex queue_mutex_;
    std::deque<std::string> output_queue_;

    void output_callback(const std::string& data);
};
