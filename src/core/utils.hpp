#pragma once

#include <string>

// Get the cluster username from the credential store.
// Returns "unknown" if credentials are not available.
std::string get_cluster_username();

// Generate an ISO 8601 timestamp (YYYY-MM-DDTHH:MM:SS) for the current local time.
std::string now_iso();

// Parse an ISO 8601 timestamp to time_t. Returns 0 on failure.
std::time_t parse_iso_time(const std::string& iso);

// Safe integer parse: returns fallback on failure (no exceptions).
int safe_stoi(const std::string& s, int fallback = 0);
