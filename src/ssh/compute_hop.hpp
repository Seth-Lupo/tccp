#pragma once

#include "connection.hpp"
#include <string>

// Lightweight wrapper that runs commands on a compute node via SSH hop
// through the DTN connection. Analogous to MultiplexedLoginHop but
// without owning a channel — just wraps commands in an SSH hop.
class ComputeHop {
public:
    ComputeHop(SSHConnection& dtn, std::string compute_node);

    SSHResult run(const std::string& cmd, int timeout_secs = 0);

    const std::string& node() const { return compute_node_; }

private:
    SSHConnection& dtn_;
    std::string compute_node_;
};

// Escape a string for safe single-quote wrapping in SSH commands.
// Shared by ComputeHop and MultiplexedLoginHop.
std::string escape_for_ssh(const std::string& cmd);
