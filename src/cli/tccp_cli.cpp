#include "tccp_cli.hpp"
#include "preflight.hpp"
#include "theme.hpp"
#include <iostream>
#include <sstream>
#include <chrono>
#include <fmt/format.h>
#include <filesystem>
#include <replxx.hxx>
#include <managers/job_manager.hpp>

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

    add_command("pwd", [](BaseCLI& cli, const std::string& arg) {
        std::cout << std::filesystem::current_path().string() << "\n";
    }, "Print working directory");

    add_command("refresh", [](BaseCLI& cli, const std::string& arg) {
        cli.service.reload_config();
        if (cli.service.has_config()) {
            std::cout << theme::ok("Reloaded tccp.yaml");
        } else {
            std::cout << theme::fail("Failed to reload tccp.yaml");
        }
    }, "Reload tccp.yaml config");

    register_connection_commands(*this);
    register_jobs_commands(*this);
    register_shell_commands(*this);
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
            if (issue.is_hint) {
                std::cout << theme::step(issue.message);
                std::cout << theme::step(issue.fix);
            } else {
                std::cout << theme::fail(issue.message);
                std::cout << theme::step(issue.fix);
            }
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
    start_monitor(rx);

    std::string line;
    while (true) {
        if (connection_lost_) break;

        current_prompt_ = get_prompt_string();
        const char* raw = rx.input(current_prompt_);
        if (!raw) {
            break;
        }

        if (connection_lost_) break;

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

        // Pause monitor during command execution to avoid interleaved output
        stop_monitor();
        execute_command(command, args);

        // Sync snapshots so we don't re-notify on state changes
        // the user just caused (run, cancel, view+detach, etc.)
        sync_snapshots();

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
                    start_monitor(rx);
                    continue;
                }

                auto cb = [](const std::string& msg) {
                    std::cout << theme::dim(msg) << "\n";
                };
                std::cout << "\n";
                service.graceful_shutdown(cb);
                std::cout << "\n";
            } else {
                if (service.alloc_manager()) {
                    service.alloc_manager()->deallocate_all_idle(nullptr);
                }
                std::cout << theme::dim("Disconnecting...") << "\n";
                service.disconnect();
            }
            break;
        }

        // Resume monitor for next input wait
        start_monitor(rx);
    }

    stop_monitor();

    if (!quit_requested_) {
        int init_count = service.initializing_job_count();
        if (init_count > 0) {
            auto cb = [](const std::string& msg) {
                std::cout << theme::dim(msg) << "\n";
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

// ── Job monitor ────────────────────────────────────────────

void TCCPCLI::start_monitor(replxx::Replxx& rx) {
    if (monitor_running_) return;
    rx_ptr_ = &rx;
    monitor_running_ = true;
    monitor_thread_ = std::thread(&TCCPCLI::monitor_loop, this);
}

void TCCPCLI::stop_monitor() {
    if (!monitor_running_) return;
    monitor_running_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    rx_ptr_ = nullptr;
}

void TCCPCLI::monitor_loop() {
    auto last_poll = std::chrono::steady_clock::now();
    auto last_health_check = std::chrono::steady_clock::now();

    while (monitor_running_) {
        auto now = std::chrono::steady_clock::now();

        // Health check every 30s
        if ((now - last_health_check) >= std::chrono::seconds(30)) {
            last_health_check = now;

            if (!service.check_alive() || !service.check_slurm_health()) {
                connection_lost_ = true;
                if (rx_ptr_) {
                    std::string output = "\r\033[2K";
                    output += theme::fail("Connection lost. Press Enter to exit.");
                    rx_ptr_->write(output.c_str(), static_cast<int>(output.size()));
                }
                return;
            }
        }

        // Poll every 10s — SSH check for completions + local init transitions
        if ((now - last_poll) >= std::chrono::seconds(10)) {
            last_poll = now;

            auto result = service.poll_jobs();

            if (!result.events.empty() && rx_ptr_) {
                std::string output = "\r\033[2K";
                for (const auto& ev : result.events) {
                    switch (ev.type) {
                    case PollResult::Event::STARTED:
                        output += theme::info(fmt::format(
                            "Job '{}' is now running on {}", ev.job_name, ev.compute_node));
                        break;
                    case PollResult::Event::COMPLETED:
                        output += theme::info(fmt::format(
                            "Job '{}' completed", ev.job_name));
                        break;
                    case PollResult::Event::FAILED:
                        output += theme::info(fmt::format(
                            "Job '{}' failed (exit {})", ev.job_name, ev.exit_code));
                        break;
                    case PollResult::Event::CANCELED:
                        output += theme::info(fmt::format(
                            "Job '{}' canceled", ev.job_name));
                        break;
                    case PollResult::Event::INIT_ERROR:
                        output += theme::info(fmt::format(
                            "Job '{}' aborted: {}", ev.job_name, ev.error));
                        break;
                    }
                }
                rx_ptr_->write(output.c_str(), static_cast<int>(output.size()));
                rx_ptr_->set_prompt(current_prompt_);
            }
        }

        // Sleep in small increments so stop_monitor() returns quickly
        for (int i = 0; i < 10 && monitor_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// Mark current tracked jobs as announced so the monitor
// doesn't re-notify after user-initiated actions.
void TCCPCLI::sync_snapshots() {
    if (!service.job_manager()) return;
    service.job_manager()->mark_all_announced();
}
