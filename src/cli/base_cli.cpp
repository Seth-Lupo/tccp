#include "base_cli.hpp"
#include "theme.hpp"
#include <iostream>
#include <fmt/format.h>

BaseCLI::BaseCLI() {
    // Config is loaded by TccpService constructor
}

void BaseCLI::add_command(const std::string& name,
                         CommandHandler handler,
                         const std::string& help) {
    commands_[name] = {handler, help};
}

bool BaseCLI::require_config() {
    if (!service.has_config()) {
        std::cout << theme::fail("Project not configured. Run 'tccp init' first.");
        return false;
    }
    return true;
}

bool BaseCLI::require_connection() {
    if (!require_config()) {
        return false;
    }
    if (!service.is_connected()) {
        std::cout << theme::fail("Not connected.");
        return false;
    }
    return true;
}

void BaseCLI::execute_command(const std::string& command, const std::string& args) {
    auto it = commands_.find(command);
    if (it == commands_.end()) {
        std::cout << theme::error("Unknown command: " + command);
        std::cout << theme::dim("Type 'help' for available commands.") << "\n";
        return;
    }

    try {
        it->second.first(*this, args);
    } catch (const std::exception& e) {
        std::cout << theme::error(std::string(e.what()));
    }
}

void BaseCLI::print_help() const {
    // Group commands by category
    std::vector<std::pair<std::string, std::vector<std::string>>> categories = {
        {"Jobs",         {"run", "view", "jobs", "cancel", "return", "restart"}},
        {"Output",       {"logs", "tail", "output", "initlogs"}},
        {"Info",         {"status", "info", "config", "allocs", "gpus"}},
        {"Remote",       {"ssh", "open", "shell", "exec", "dealloc"}},
        {"General",      {"help", "clear", "quit"}},
#ifdef TCCP_DEV
        {"Dev",          {"debug"}},
#endif
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
    // Replxx handles ANSI escapes natively — no \001/\002 wrapping needed.
    if (!service.has_config()) {
        return std::string(theme::color::BROWN) + "tccp"
             + theme::color::RESET + "> ";
    }
    return std::string(theme::color::BLUE) + service.config().project().name
         + theme::color::RESET + "@"
         + theme::color::BROWN + "tccp"
         + theme::color::RESET + "> ";
}
