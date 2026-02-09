#include "base_cli.hpp"
#include "theme.hpp"
#include <iostream>
#include <fmt/format.h>

BaseCLI::BaseCLI() {
    auto config_result = Config::load();
    if (config_result.is_ok()) {
        config = config_result.value;
    }
}

void BaseCLI::add_command(const std::string& name,
                         CommandHandler handler,
                         const std::string& help) {
    commands_[name] = {handler, help};
}

bool BaseCLI::require_config() {
    if (!config.has_value()) {
        std::cout << theme::fail("Project not configured. Run 'tccp init' first.");
        return false;
    }
    return true;
}

bool BaseCLI::require_connection() {
    if (!require_config()) {
        return false;
    }
    if (!cluster || !cluster->is_connected()) {
        std::cout << theme::fail("Not connected.");
        return false;
    }
    return true;
}

void BaseCLI::init_managers() {
    if (config && cluster && cluster->is_connected()) {
        jobs = std::make_unique<JobManager>(config.value(), cluster->dtn(), cluster->login());
    }
}

void BaseCLI::clear_managers() {
    jobs.reset();
}

void BaseCLI::execute_command(const std::string& command, const std::string& args) {
    auto it = commands_.find(command);
    if (it == commands_.end()) {
        std::cout << theme::fail("Unknown command: " + command);
        std::cout << theme::step("Type 'help' for available commands.");
        return;
    }

    try {
        it->second.first(*this, args);
    } catch (const std::exception& e) {
        std::cout << theme::fail(std::string(e.what()));
    }
}

void BaseCLI::print_help() const {
    // Group commands by category
    std::vector<std::pair<std::string, std::vector<std::string>>> categories = {
        {"Connection", {"status", "disconnect"}},
        {"Jobs",       {"run", "view", "jobs", "cancel", "return"}},
        {"Shell",      {"shell", "exec"}},
        {"Setup",      {"init", "register", "credentials"}},
        {"General",    {"help", "quit", "exit"}},
    };

    for (const auto& [cat_name, cmd_names] : categories) {
        bool has_any = false;
        for (const auto& name : cmd_names) {
            if (commands_.count(name)) {
                has_any = true;
                break;
            }
        }
        if (!has_any) continue;

        std::cout << "\n" << theme::color::BROWN << theme::color::BOLD
                  << "  " << cat_name << theme::color::RESET << "\n";

        for (const auto& name : cmd_names) {
            auto it = commands_.find(name);
            if (it != commands_.end()) {
                std::cout << theme::color::BLUE
                          << fmt::format("    {:<14}", name)
                          << theme::color::RESET
                          << theme::color::DIM
                          << it->second.second
                          << theme::color::RESET << "\n";
            }
        }
    }
    std::cout << "\n";
}

std::string BaseCLI::get_prompt_string() const {
    // Readline uses \001 and \002 to wrap non-printing chars so it can
    // compute the visible prompt width correctly for cursor positioning.
    auto rl_esc = [](const std::string& code) {
        return std::string("\001") + code + std::string("\002");
    };

    if (!config.has_value()) {
        return rl_esc(theme::color::BROWN) + "tccp"
             + rl_esc(theme::color::RESET) + "> ";
    } else if (cluster && cluster->is_connected()) {
        return rl_esc(theme::color::BROWN) + "tccp"
             + rl_esc(theme::color::RESET) + ":"
             + rl_esc(theme::color::BLUE) + config.value().project().name
             + rl_esc(theme::color::RESET) + "@"
             + rl_esc(theme::color::GREEN) + "cluster"
             + rl_esc(theme::color::RESET) + "> ";
    } else {
        return rl_esc(theme::color::BROWN) + "tccp"
             + rl_esc(theme::color::RESET) + ":"
             + rl_esc(theme::color::BLUE) + config.value().project().name
             + rl_esc(theme::color::RESET) + "> ";
    }
}
