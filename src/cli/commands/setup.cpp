#include "../base_cli.hpp"
#include <iostream>

static void do_init(BaseCLI& cli, const std::string& arg) {
    std::cout << "Initialization not yet implemented.\n";
}

static void do_register(BaseCLI& cli, const std::string& arg) {
    std::cout << "Project registration not yet implemented.\n";
}

void register_setup_commands(BaseCLI& cli) {
    cli.add_command("init", do_init, "Initialize global configuration");
    cli.add_command("register", do_register, "Register current directory as project");
}
