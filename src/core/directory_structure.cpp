#include "directory_structure.hpp"
#include <cstdlib>

fs::path get_tccp_root() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path(home) / ".tccp";
}

void ensure_tccp_directory_structure() {
    fs::path root = get_tccp_root();

    // Create base directories
    fs::create_directories(root);
    fs::create_directories(root / "projects");
    fs::create_directories(root / "state");
    fs::create_directories(root / "allocations");
}

void ensure_project_directories(const std::string& project_name) {
    fs::path root = get_tccp_root();

    // Create project-specific directories
    fs::path project_root = root / "projects" / project_name;
    fs::create_directories(project_root / "jobs");
}
