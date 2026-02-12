#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Ensures the base ~/.tccp directory structure exists
// Creates directories as needed but does not overwrite files
void ensure_tccp_directory_structure();

// Ensures project-specific directories exist
void ensure_project_directories(const std::string& project_name);

// Get the base ~/.tccp path
fs::path get_tccp_root();
