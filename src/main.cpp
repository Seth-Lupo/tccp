#include <iostream>
#include <vector>
#include <string>
#include "cli/tccp_cli.hpp"
#include "cli/theme.hpp"

void print_usage() {
    std::cout << theme::banner();
    std::cout << theme::section("Usage");
    std::cout << theme::color::BLUE << "    tccp"
              << theme::color::RESET << theme::color::DIM
              << "                  Connect and enter REPL" << theme::color::RESET << "\n";
    std::cout << theme::color::BLUE << "    tccp connect"
              << theme::color::RESET << theme::color::DIM
              << "          Connect and enter REPL" << theme::color::RESET << "\n";
    std::cout << theme::color::BLUE << "    tccp setup"
              << theme::color::RESET << theme::color::DIM
              << "            Store cluster credentials" << theme::color::RESET << "\n";
    std::cout << theme::color::BLUE << "    tccp new "
              << theme::color::RESET << theme::color::BROWN << "<template>"
              << theme::color::RESET << theme::color::DIM
              << "   Create new project" << theme::color::RESET << "\n";
    std::cout << theme::color::BLUE << "    tccp register "
              << theme::color::RESET << theme::color::BROWN << "[path]"
              << theme::color::RESET << theme::color::DIM
              << "    Register a project directory" << theme::color::RESET << "\n";
    std::cout << theme::color::BLUE << "    tccp manual "
              << theme::color::RESET << theme::color::BROWN << "[topic]"
              << theme::color::RESET << theme::color::DIM
              << "     Show manual (python, python-pytorch)" << theme::color::RESET << "\n";
    std::cout << "\n";
    std::cout << theme::color::DIM
              << "    tccp --version        Show version\n"
              << "    tccp --help           Show this help"
              << theme::color::RESET << "\n\n";
}

int main(int argc, char** argv) {
    try {
        TCCPCLI cli;

        if (argc == 1) {
            cli.run_connected_repl();
        } else {
            std::string cmd = argv[1];

            if (cmd == "--version") {
                std::cout << theme::color::BROWN << theme::color::BOLD << "tccp"
                          << theme::color::RESET << theme::color::DIM
                          << " version 0.3.0" << theme::color::RESET << "\n";
                return 0;
            } else if (cmd == "--help") {
                print_usage();
                return 0;
            } else if (cmd == "connect") {
                cli.run_connected_repl();
            } else if (cmd == "register") {
                cli.run_register(argc >= 3 ? argv[2] : "");
            } else if (cmd == "manual") {
                cli.run_manual(argc >= 3 ? argv[2] : "");
            } else if (cmd == "setup") {
                cli.run_setup();
            } else if (cmd == "new") {
                if (argc < 3) {
                    std::cout << theme::fail("Missing template name.");
                    std::cout << theme::step("Usage: tccp new <template>");
                    std::cout << theme::step("Available templates: python, qwen");
                    return 1;
                }
                cli.run_new(argv[2]);
            } else {
                std::cout << theme::fail("Unknown command: " + cmd);
                print_usage();
                return 1;
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cout << theme::fail(std::string(e.what()));
        return 1;
    }
}
