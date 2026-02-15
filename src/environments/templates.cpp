#include "templates.hpp"
#include <unordered_map>

// Defined in python_template.cpp / qwen_template.cpp
ProjectTemplate make_python_template();
ProjectTemplate make_qwen_template();

static const std::unordered_map<std::string, ProjectTemplate>& registry() {
    static const std::unordered_map<std::string, ProjectTemplate> templates = {
        {"python", make_python_template()},
        {"qwen", make_qwen_template()},
    };
    return templates;
}

const ProjectTemplate* get_template(const std::string& name) {
    auto& templates = registry();
    auto it = templates.find(name);
    if (it == templates.end()) return nullptr;
    return &it->second;
}

std::vector<std::string> list_templates() {
    auto& templates = registry();
    std::vector<std::string> names;
    names.reserve(templates.size());
    for (auto& [name, _] : templates) {
        names.push_back(name);
    }
    return names;
}
