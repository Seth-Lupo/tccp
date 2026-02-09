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
#include <core/credentials.hpp>
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
        std::cout << theme::dim("Disconnecting...") << "\n";
        exit(0);
    }, "Exit TCCP");

    add_command("exit", [](BaseCLI& cli, const std::string& arg) {
        std::cout << theme::dim("Disconnecting...") << "\n";
        exit(0);
    }, "Exit TCCP");

    register_connection_commands(*this);
    register_jobs_commands(*this);
    register_shell_commands(*this);
    register_setup_commands(*this);
    register_credentials_commands(*this);
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
    std::cout << theme::ok("Credentials loaded");
    std::cout << theme::ok("Global config loaded");
    std::cout << theme::ok("Project config loaded");

    // Reload config (preflight confirmed it's valid)
    auto config_result = Config::load();
    if (config_result.is_err()) {
        std::cout << theme::fail(config_result.error) << "\n";
        return;
    }
    config = config_result.value;

    // Connect
    std::cout << theme::section("Connecting");
    cluster = std::make_unique<ClusterConnection>(config.value());

    auto callback = [](const std::string& msg) {
        std::cout << theme::info(msg);
    };

    auto result = cluster->connect(callback);
    if (result.failed()) {
        std::cout << theme::fail("Connection failed: " + result.stderr_data);
        std::cout << theme::dim("    If you're off campus, make sure the Tufts VPN is connected.") << "\n\n";
        cluster.reset();
        return;
    }

    // Initialize managers
    init_managers();

    std::cout << theme::section("Connected");
    std::cout << theme::kv("DTN", config.value().dtn().host);
    std::cout << theme::kv("Login", config.value().login().host);
    std::cout << theme::kv("Project", config.value().project().name);
    std::cout << theme::divider();
    std::cout << theme::dim("    Type 'help' for commands, 'quit' to exit.") << "\n\n";

    // REPL loop — lifetime is bound to the DTN connection
    std::string line;
    while (true) {
        // Check connection health before prompting
        if (!cluster || !cluster->check_alive()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("Connection to cluster lost.");
            std::cout << theme::dim("    The SSH session to the DTN has ended.") << "\n";
            std::cout << theme::dim("    This can happen due to network changes, VPN disconnects,") << "\n";
            std::cout << theme::dim("    or server-side timeouts.") << "\n\n";
            std::cout << theme::step("Run 'tccp' to reconnect.");
            std::cout << "\n";
            break;
        }

        // Poll for completed jobs (throttled to every 30s)
        auto now = std::chrono::steady_clock::now();
        if (jobs && (now - last_poll_time_) >= std::chrono::seconds(30)) {
            last_poll_time_ = now;
            jobs->poll([this](const TrackedJob& tj) {
                std::cout << "\n" << theme::ok(
                    "Job '" + tj.job_name + "' (" + tj.slurm_id + ") completed.");

                // Auto-return output
                auto cb = [](const std::string& msg) {
                    std::cout << theme::ok(msg);
                };
                jobs->return_output(tj.job_id, cb);
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

        // Check connection after each command
        if (cluster && !cluster->check_alive()) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("Connection to cluster lost.");
            std::cout << theme::dim("    The SSH session to the DTN has ended.") << "\n";
            std::cout << theme::dim("    This can happen due to network changes, VPN disconnects,") << "\n";
            std::cout << theme::dim("    or server-side timeouts.") << "\n\n";
            std::cout << theme::step("Run 'tccp' to reconnect.");
            std::cout << "\n";
            break;
        }
    }

    // Cleanup
    clear_managers();
    if (cluster) {
        cluster->disconnect();
        cluster.reset();
    }
}

void TCCPCLI::run_register() {
    std::cout << "Project registration not yet implemented.\n";
}

static bool write_if_missing(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path)) {
        std::cout << theme::dim("    ~ " + path.filename().string() + " (already exists)") << "\n";
        return false;
    }
    std::ofstream f(path);
    f << content;
    f.close();
    std::cout << theme::ok("Created " + path.filename().string());
    return true;
}

void TCCPCLI::run_new(const std::string& template_name) {
    if (template_name != "python") {
        std::cout << theme::fail("Unknown template: " + template_name);
        std::cout << theme::step("Available templates: python");
        return;
    }

    auto cwd = std::filesystem::current_path();
    std::string dirname = cwd.filename().string();

    std::cout << theme::banner();
    std::cout << theme::section("New Python Project");
    std::cout << theme::kv("Directory", cwd.string());
    std::cout << "\n";

    // tccp.yaml
    std::string yaml =
        "name: " + dirname + "\n"
        "type: python\n"
        "\n"
        "rodata:\n"
        "  dataset: ./data\n"
        "\n"
        "jobs:\n"
        "  main:\n"
        "    script: main.py\n"
        "    args: \"\"\n";
    write_if_missing(cwd / "tccp.yaml", yaml);

    // main.py
    std::string main_py =
        "import numpy as np\n"
        "import os\n"
        "import time\n"
        "\n"
        "# ── Step 1: Load data ──\n"
        "print(\"[step 1] Loading data...\")\n"
        "time.sleep(1)\n"
        "data = np.loadtxt(\"rodata/dataset/input.csv\", delimiter=\",\")\n"
        "print(f\"  Loaded {data.shape[0]} rows, {data.shape[1]} cols\")\n"
        "print(f\"  Column means: {data.mean(axis=0)}\")\n"
        "print(f\"  Column ranges: {data.min(axis=0)} -> {data.max(axis=0)}\")\n"
        "print()\n"
        "\n"
        "# ── Step 2: Ask for confirmation ──\n"
        "response = input(\"[step 2] Normalize and write output? (yes/no): \")\n"
        "if response.strip().lower() != \"yes\":\n"
        "    print(\"Aborted.\")\n"
        "    exit(0)\n"
        "print()\n"
        "\n"
        "# ── Step 3: Process and write ──\n"
        "print(\"[step 3] Normalizing...\")\n"
        "time.sleep(1)\n"
        "mins = data.min(axis=0)\n"
        "maxs = data.max(axis=0)\n"
        "normalized = (data - mins) / (maxs - mins + 1e-8)\n"
        "print(f\"  Column means after: {normalized.mean(axis=0)}\")\n"
        "\n"
        "os.makedirs(\"output\", exist_ok=True)\n"
        "np.savetxt(\"output/normalized.csv\", normalized, delimiter=\",\", fmt=\"%.6f\")\n"
        "print(f\"  Wrote output/normalized.csv ({normalized.shape[0]} rows)\")\n"
        "print()\n"
        "print(\"Done.\")\n";
    write_if_missing(cwd / "main.py", main_py);

    // Sample input data
    std::filesystem::create_directories(cwd / "data");
    std::string input_csv =
        "1.0,10.0,100.0\n"
        "2.0,20.0,200.0\n"
        "3.0,30.0,300.0\n"
        "4.0,40.0,400.0\n"
        "5.0,50.0,500.0\n";
    write_if_missing(cwd / "data" / "input.csv", input_csv);

    // requirements.txt
    std::string requirements =
        "numpy\n";
    write_if_missing(cwd / "requirements.txt", requirements);

    // .gitignore
    std::string gitignore =
        "__pycache__/\n"
        "*.pyc\n"
        ".venv/\n"
        "*.egg-info/\n"
        ".env\n"
        "tccp-output/\n";
    write_if_missing(cwd / ".gitignore", gitignore);

    std::cout << theme::divider();
    std::cout << theme::dim("    Project ready.") << "\n";
    std::cout << theme::step("Run 'tccp' to connect and 'run <job>' to submit.") << "\n";
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
#ifdef TCCP_DEV
    std::cout << theme::dim("    Credentials saved to ~/.tccp/credentials.") << "\n";
#else
    std::cout << theme::dim("    Credentials saved to system keychain.") << "\n";
#endif
    std::cout << theme::step("Run 'tccp' or 'tccp connect' to connect.") << "\n";
}

void TCCPCLI::run_command(const std::string& command, const std::vector<std::string>& args) {
    std::string args_str;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) args_str += " ";
        args_str += args[i];
    }
    execute_command(command, args_str);
}
