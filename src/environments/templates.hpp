#pragma once

#include <string>
#include <vector>

struct TemplateFile {
    std::string path;       // relative path, e.g. "chat.py", "data/input.csv"
    std::string content;
};

struct ProjectTemplate {
    std::string name;              // "python", "qwen"
    std::string project_type;      // maps to EnvironmentConfig type
    std::string description;       // for UI display
    std::vector<TemplateFile> files;
    std::vector<std::string> directories;   // directories to create (e.g. "output")
    std::vector<std::string> next_steps;    // UI next-step messages
};

// Returns nullptr if template name is unknown.
const ProjectTemplate* get_template(const std::string& name);

// Returns list of available template names.
std::vector<std::string> list_templates();
