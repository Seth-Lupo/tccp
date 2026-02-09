#include "../base_cli.hpp"
#include <iostream>

static void do_credentials(BaseCLI& cli, const std::string& arg) {
    std::cout << "Credential management not yet implemented.\n";
}

void register_credentials_commands(BaseCLI& cli) {
    cli.add_command("credentials", do_credentials, "Manage stored credentials");
}
