#include "../base_cli.hpp"
#include <iostream>

static void do_exec(BaseCLI& cli, const std::string& arg) {
    if (!cli.require_connection()) return;
    if (arg.empty()) {
        std::cout << "Usage: exec <command>\n";
        return;
    }
    auto result = cli.service.exec_remote(arg);
    std::cout << result.stdout_data;
    if (!result.stderr_data.empty()) {
        std::cerr << result.stderr_data;
    }
}

void register_shell_commands(BaseCLI& cli) {
    cli.add_command("exec", do_exec, "Execute remote command on login node");
}
