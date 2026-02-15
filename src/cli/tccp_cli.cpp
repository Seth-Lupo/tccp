#include "tccp_cli.hpp"
#include "preflight.hpp"
#include "theme.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/directory_structure.hpp>
#include <environments/templates.hpp>
#include <readline/readline.h>
#include <readline/history.h>

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
            // Reset in-memory state without destroying managers
            // (background init threads hold references to the managers)
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
    std::cout << theme::check("Credentials loaded");
    std::cout << theme::check("Global config loaded");
    std::cout << theme::check("Project config loaded");

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
    std::string line;
    while (true) {
        // Check connection health before prompting
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

        // Check SLURM health before each command
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

                // Auto-return output
                auto cb = [](const std::string& msg) {
                    std::cout << theme::dim(msg) << "\n";
                };
                service.return_output(tj.job_id, cb);
            });
        }

        std::string prompt = get_prompt_string();
        char* raw = readline(prompt.c_str());
        if (!raw) {
            break;  // EOF / Ctrl-D
        }

        line = raw;
        free(raw);

        if (line.empty()) {
            continue;
        }

        add_history(line.c_str());

        std::istringstream iss(line);
        std::string command;
        iss >> command;

        std::string args;
        std::getline(iss, args);
        if (!args.empty() && args[0] == ' ') {
            args = args.substr(1);
        }

        execute_command(command, args);

        // Handle quit/exit request
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
                std::cout << theme::dim("Disconnecting...") << "\n";
                service.disconnect();
            }
            break;
        }

        // Check connection after each command
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

        // Check SLURM health after each command
        if (service.is_connected() && !service.check_slurm_health()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("SLURM stopped responding.");
            std::cout << theme::dim("    The cluster has become unavailable.") << "\n";
            std::cout << theme::dim("    Run 'exec sinfo' to test connectivity.") << "\n\n";
            std::cout << theme::dim("    Exiting. Run 'tccp' when the cluster is healthy.") << "\n\n";
            break;
        }
    }

    // Cleanup — if we broke out of the loop without quit_requested_ (Ctrl+D,
    // connection loss), still do graceful shutdown for any init jobs.
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

void TCCPCLI::run_register() {
    std::cout << "Project registration not yet implemented.\n";
}

static bool write_if_missing(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path)) {
        std::cout << theme::dim("    ~ ") << theme::dim(path.filename().string())
                  << theme::dim(" (exists)") << "\n";
        return false;
    }
    std::ofstream f(path);
    f << content;
    f.close();
    std::cout << theme::green("    + ") << path.filename().string() << "\n";
    return true;
}

// Replace {{PROJECT_NAME}} in template content
static std::string substitute(const std::string& text, const std::string& project_name) {
    std::string result = text;
    const std::string placeholder = "{{PROJECT_NAME}}";
    std::string::size_type pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
        result.replace(pos, placeholder.size(), project_name);
        pos += project_name.size();
    }
    return result;
}

void TCCPCLI::run_new(const std::string& template_name) {
    const auto* tmpl = get_template(template_name);
    if (!tmpl) {
        std::cout << theme::fail("Unknown template: " + template_name);
        auto names = list_templates();
        std::sort(names.begin(), names.end());
        std::string available;
        for (size_t i = 0; i < names.size(); i++) {
            if (i > 0) available += ", ";
            available += names[i];
        }
        std::cout << theme::step("Available templates: " + available);
        return;
    }

    auto cwd = std::filesystem::current_path();
    std::string dirname = cwd.filename().string();

    std::cout << theme::banner();
    std::cout << theme::section("New " + tmpl->description);
    std::cout << theme::kv("Directory", cwd.string());
    std::cout << "\n  " << theme::dim("Files") << "\n";

    // Write template files
    for (const auto& file : tmpl->files) {
        std::string content = substitute(file.content, dirname);
        std::filesystem::path fpath = cwd / file.path;
        // Create parent directories if needed (e.g. "data/input.csv")
        std::filesystem::create_directories(fpath.parent_path());
        write_if_missing(fpath, content);
    }

    // Create directories
    for (const auto& dir : tmpl->directories) {
        std::filesystem::create_directories(cwd / dir);
    }

    // Print next steps
    std::cout << theme::divider();
    std::cout << theme::section("Next Steps");
    for (size_t i = 0; i < tmpl->next_steps.size(); i++) {
        std::cout << "    " << theme::white(std::to_string(i + 1) + ".")
                  << " " << tmpl->next_steps[i] << "\n";
    }
    std::cout << "\n";
}

static std::string read_password(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::string password;
    std::getline(std::cin, password);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << "\n";
    return password;
}

void TCCPCLI::run_setup() {
    // Ensure directory structure exists
    ensure_tccp_directory_structure();

    // Create default config file if it doesn't exist
    auto config_result = create_default_global_config();
    if (config_result.is_err()) {
        std::cout << theme::fail("Failed to create config file: " + config_result.error);
        return;
    }

    std::cout << theme::banner();
    std::cout << theme::section("Cluster Setup");
    std::cout << theme::dim("    One username and password for the entire Tufts cluster network.") << "\n";
#ifdef TCCP_DEV
    std::cout << theme::dim("    Credentials are stored in ~/.tccp/credentials.") << "\n\n";
#else
    std::cout << theme::dim("    Credentials are stored securely in the system keychain.") << "\n\n";
#endif

    auto& creds = CredentialManager::instance();

    // Username
    std::string user;
    std::cout << theme::color::BROWN << "    Tufts username: " << theme::color::RESET;
    std::cout.flush();
    std::getline(std::cin, user);

    if (user.empty()) {
        std::cout << theme::fail("Username cannot be empty.");
        return;
    }

    // Password
    std::string password = read_password(
        theme::color::BROWN + "    Tufts password: " + theme::color::RESET
    );
    if (password.empty()) {
        std::cout << theme::fail("Password cannot be empty.");
        return;
    }

    // Email
    std::string email;
    std::cout << theme::color::BROWN << "    Email (for job notifications): " << theme::color::RESET;
    std::cout.flush();
    std::getline(std::cin, email);

    // Store
    auto r1 = creds.set("user", user);
    auto r2 = creds.set("password", password);
    auto r3 = creds.set("email", email);

    if (r1.is_err() || r2.is_err()) {
        std::cout << "\n" << theme::fail("Failed to store credentials.");
        return;
    }

    std::cout << theme::divider();
    std::cout << theme::ok("    Config file ready at ~/.tccp/config.yaml.");
#ifdef TCCP_DEV
    std::cout << theme::ok("    Credentials saved to ~/.tccp/credentials.");
#else
    std::cout << theme::ok("    Credentials saved to system keychain.");
#endif
    std::cout << theme::ok("    Run 'tccp' or 'tccp connect' to connect.");
    std::cout << "\n";
}

void TCCPCLI::run_command(const std::string& command, const std::vector<std::string>& args) {
    std::string args_str;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) args_str += " ";
        args_str += args[i];
    }
    execute_command(command, args_str);
}
