#include "../base_cli.hpp"
#include "../theme.hpp"
#include <iostream>
#include <fmt/format.h>
#include <core/config.hpp>

static void do_disconnect(BaseCLI& cli, const std::string& arg) {
    if (!cli.service.is_connected()) {
        std::cout << theme::error("Not connected.");
        return;
    }

    cli.service.disconnect();
    std::cout << theme::dim("    Disconnecting...") << "\n";
    exit(0);
}

static void do_status(BaseCLI& cli, const std::string& arg) {
    std::cout << theme::section("Status");

    // Global config
    if (global_config_exists()) {
        std::cout << theme::kv("Config", get_global_config_path().string());
    } else {
        std::cout << theme::error("Config not found");
    }

    // Project
    if (cli.service.has_config()) {
        std::cout << theme::kv("Project", cli.service.config().project().name);
        std::cout << theme::kv("Remote", cli.service.config().project().remote_dir);
    } else {
        std::cout << theme::error("Project not registered");
    }

    // Connection
    if (cli.service.is_connected()) {
        std::cout << theme::kv("DTN", "connected");
        std::cout << theme::kv("Login", "connected");
    } else {
        std::cout << theme::kv("DTN", "not connected");
        std::cout << theme::kv("Login", "not connected");
    }

    std::cout << "\n";
}

void register_connection_commands(BaseCLI& cli) {
    cli.add_command("disconnect", do_disconnect, "Disconnect and exit");
    cli.add_command("status", do_status, "Show connection and project status");
}
