#include "directory_structure.hpp"
#include <platform/platform.hpp>

fs::path get_tccp_root() {
    return platform::home_dir() / ".tccp";
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
