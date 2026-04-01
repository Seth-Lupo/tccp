#include "compute_hop.hpp"
#include <core/constants.hpp>
#include <fmt/format.h>

ComputeHop::ComputeHop(SSHConnection& dtn, std::string compute_node)
    : dtn_(dtn), compute_node_(std::move(compute_node)) {}

SSHResult ComputeHop::run(const std::string& cmd, int timeout_secs) {
    std::string wrapped = fmt::format("ssh {} {} {} </dev/null",
                                       SSH_OPTS, compute_node_,
                                       escape_for_ssh(cmd));
    return dtn_.run(wrapped, timeout_secs);
}

std::string escape_for_ssh(const std::string& cmd) {
    std::string escaped;
    escaped.reserve(cmd.size() + 10);
    escaped += '\'';
    for (char c : cmd) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += '\'';
    return escaped;
}
