#pragma once

#include <string>
#include <vector>
#include <core/types.hpp>

struct PreflightIssue {
    std::string message;
    std::string fix;
    bool is_hint = false;  // true = friendly nudge, false = error
};

// Runs all preflight checks before connecting.
// Returns empty vector if everything is good.
std::vector<PreflightIssue> run_preflight_checks();

// Individual checks (for granular use)
std::vector<PreflightIssue> check_credentials();
std::vector<PreflightIssue> check_global_config();
std::vector<PreflightIssue> check_project_config();
