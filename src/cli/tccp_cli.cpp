#include "tccp_cli.hpp"
#include "preflight.hpp"
#include "theme.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <fmt/format.h>
#include <replxx.hxx>

TCCPCLI::TCCPCLI() : BaseCLI() {
    register_all_commands();
}

void TCCPCLI::register_all_commands() {
    add_command("help", [this](BaseCLI& cli, const std::string& arg) {
        this->print_help();
    }, "Show this help message");

    add_command("quit", [](BaseCLI& cli, const std::string& arg) {
        cli.quit_requested_ = true;
    }, "Exit TCCP");

    add_command("exit", [](BaseCLI& cli, const std::string& arg) {
        cli.quit_requested_ = true;
    }, "Exit TCCP");

    add_command("clear", [](BaseCLI& cli, const std::string& arg) {
        std::cout << "\033[2J\033[H" << std::flush;
    }, "Clear the screen");

    add_command("refresh", [](BaseCLI& cli, const std::string& arg) {
        cli.service.reload_config();
        if (cli.service.has_config()) {
            std::cout << theme::check("Reloaded tccp.yaml");
        } else {
            std::cout << theme::fail("Failed to reload tccp.yaml");
        }
    }, "Reload tccp.yaml config");

    register_connection_commands(*this);
    register_jobs_commands(*this);
    register_shell_commands(*this);
    register_setup_commands(*this);
    register_credentials_commands(*this);
    register_allocations_commands(*this);

#ifdef TCCP_DEV
    add_command("debug", [](BaseCLI& cli, const std::string& arg) {
        if (arg == "clean-state") {
            if (!cli.service.state_store()) {
                std::cout << theme::fail("No project loaded.");
                return;
            }
            ProjectState empty;
            cli.service.state_store()->save(empty);
            if (cli.service.alloc_manager()) {
                cli.service.alloc_manager()->state() = empty;
            }
            if (cli.service.job_manager()) {
                cli.service.job_manager()->clear_tracked();
            }
            std::cout << theme::ok("State wiped. All allocations and jobs forgotten.");
        } else {
            std::cout << theme::fail("Unknown debug command: " + arg);
            std::cout << theme::step("Available: debug clean-state");
        }
    }, "Dev debugging commands");
#endif
}

void TCCPCLI::run_connected_repl() {
    std::cout << theme::banner();

    // Preflight
    std::cout << theme::section("Preflight");
    auto issues = run_preflight_checks();
    if (!issues.empty()) {
        for (const auto& issue : issues) {
            std::cout << theme::fail(issue.message);
            std::cout << theme::step(issue.fix);
        }
        std::cout << "\n";
        return;
    }
    std::cout << theme::check("Project config valid");
    std::cout << theme::check("Credentials loaded");
    std::cout << theme::check("Global config loaded");

    // Reload config (preflight confirmed it's valid)
    service.reload_config();
    if (!service.has_config()) {
        std::cout << theme::fail("Failed to load config") << "\n";
        return;
    }

    // Connect
    std::cout << theme::section("Connecting");

    auto callback = [](const std::string& msg) {
        std::cout << theme::dim("    " + msg) << "\n";
    };

    auto result = service.connect(callback);
    if (result.is_err()) {
        std::cout << theme::fail(result.error);
        std::cout << theme::dim("    If you're off campus, make sure the Tufts VPN is connected.") << "\n\n";
        return;
    }
    std::cout << theme::check("Connection is HEALTHY");

    // Check SLURM health before starting REPL
    std::cout << theme::section("Checking SLURM");
    if (!service.check_slurm_health()) {
        std::cout << theme::fail("SLURM is not responding.");
        std::cout << theme::dim("    Run 'exec sinfo' to test cluster connectivity.") << "\n";
        std::cout << theme::dim("    The cluster may be down or undergoing maintenance.") << "\n";
        std::cout << theme::dim("    Check for maintenance announcements from Tufts HPC.") << "\n\n";
        std::cout << theme::dim("    Try again later when the cluster is healthy.") << "\n\n";
        service.disconnect();
        return;
    }
    std::cout << theme::check("SLURM controller is UP");
    std::cout << "\n";

    std::cout << theme::section("Connected");
    std::cout << theme::kv("DTN", service.config().dtn().host);
    std::cout << theme::kv("Login", service.config().login().host);
    std::cout << theme::kv("Project", service.config().project().name);
    std::cout << theme::divider();
    std::cout << theme::dim("    Type 'help' for commands, 'quit' to exit.") << "\n\n";

    // REPL loop — lifetime is bound to the DTN connection
    replxx::Replxx rx;
    std::string line;
    while (true) {
        if (!service.check_alive()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("Connection to cluster lost.");
            std::cout << theme::dim("    The SSH session to the DTN has ended.") << "\n";
            std::cout << theme::dim("    This can happen due to network changes, VPN disconnects,") << "\n";
            std::cout << theme::dim("    or server-side timeouts.") << "\n\n";
            std::cout << theme::step("Run 'tccp' to reconnect.");
            std::cout << "\n";
            break;
        }

        if (!service.check_slurm_health()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("SLURM is not responding.");
            std::cout << theme::dim("    The cluster has become unavailable.") << "\n";
            std::cout << theme::dim("    Run 'exec sinfo' to test connectivity.") << "\n\n";
            std::cout << theme::dim("    Exiting. Run 'tccp' when the cluster is healthy.") << "\n\n";
            break;
        }

        // Poll for completed jobs (throttled to every 30s)
        auto now = std::chrono::steady_clock::now();
        if (service.job_manager() && (now - last_poll_time_) >= std::chrono::seconds(30)) {
            last_poll_time_ = now;
            service.poll_jobs([this](const TrackedJob& tj) {
                std::cout << "\n" << theme::dim(
                    "Job '" + tj.job_name + "' (" + tj.slurm_id + ") completed.") << "\n";

                auto cb = [](const std::string& msg) {
                    std::cout << theme::dim(msg) << "\n";
                };
                service.return_output(tj.job_id, cb);
            });
        }

        std::string prompt = get_prompt_string();
        const char* raw = rx.input(prompt);
        if (!raw) {
            break;
        }

        line = raw;

        if (line.empty()) {
            continue;
        }

        rx.history_add(line);

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        std::string args;
        std::getline(iss, args);
        if (!args.empty() && args[0] == ' ') {
            args = args.substr(1);
        }

        execute_command(command, args);

        if (quit_requested_) {
            int init_count = service.initializing_job_count();
            if (init_count > 0) {
                std::cout << "\n" << theme::yellow(fmt::format(
                    "    {} job{} still initializing.", init_count,
                    init_count == 1 ? " is" : "s are")) << "\n";
                std::cout << theme::dim("    Exiting will cancel them and release their allocations.") << "\n";
                std::cout << theme::dim("    Running jobs will continue on the cluster.") << "\n\n";
                std::cout << theme::color::BROWN << "    Continue? (y/n) " << theme::color::RESET;
                std::cout.flush();

                std::string answer;
                std::getline(std::cin, answer);
                if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
                    quit_requested_ = false;
                    std::cout << theme::dim("    Exit canceled.") << "\n\n";
                    continue;
                }

                auto cb = [](const std::string& msg) {
                    std::cout << theme::dim("    " + msg) << "\n";
                };
                std::cout << "\n";
                service.graceful_shutdown(cb);
            } else {
                if (service.alloc_manager()) {
                    service.alloc_manager()->deallocate_all_idle(nullptr);
                }
                std::cout << theme::dim("Disconnecting...") << "\n";
                service.disconnect();
            }
            break;
        }

        if (service.is_connected() && !service.check_alive()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("Connection to cluster lost.");
            std::cout << theme::dim("    The SSH session to the DTN has ended.") << "\n";
            std::cout << theme::dim("    This can happen due to network changes, VPN disconnects,") << "\n";
            std::cout << theme::dim("    or server-side timeouts.") << "\n\n";
            std::cout << theme::step("Run 'tccp' to reconnect.");
            std::cout << "\n";
            break;
        }

        if (service.is_connected() && !service.check_slurm_health()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("SLURM stopped responding.");
            std::cout << theme::dim("    The cluster has become unavailable.") << "\n";
            std::cout << theme::dim("    Run 'exec sinfo' to test connectivity.") << "\n\n";
            std::cout << theme::dim("    Exiting. Run 'tccp' when the cluster is healthy.") << "\n\n";
            break;
        }
    }

    if (!quit_requested_) {
        int init_count = service.initializing_job_count();
        if (init_count > 0) {
            auto cb = [](const std::string& msg) {
                std::cout << theme::dim("    " + msg) << "\n";
            };
            service.graceful_shutdown(cb);
        } else {
            service.disconnect();
        }
    }
}

void TCCPCLI::run_command(const std::string& command, const std::vector<std::string>& args) {
    std::string args_str;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) args_str += " ";
        args_str += args[i];
    }
    execute_command(command, args_str);
}
