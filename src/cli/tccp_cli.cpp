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
                // Explicit quit: release idle allocations so we don't hold
                // cluster resources after the user is done.
                if (service.alloc_manager()) {
                    service.alloc_manager()->deallocate_all_idle(nullptr);
                }
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

// ── Interactive prompt helpers ──────────────────────────

// Prompt for a line of input with a label. Returns empty string on EOF.
static std::string prompt_line(const std::string& label, const std::string& default_val = "") {
    std::string suffix = default_val.empty() ? ": " : " [" + default_val + "]: ";
    std::cout << theme::color::BROWN << "    " << label << suffix << theme::color::RESET;
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer)) return default_val;
    if (answer.empty()) return default_val;
    return answer;
}

// Prompt for a choice from a numbered list. Returns 0-based index.
static int prompt_choice(const std::string& label,
                         const std::vector<std::pair<std::string, std::string>>& options,
                         int default_idx = 0) {
    std::cout << "\n" << theme::dim("    " + label) << "\n";
    for (size_t i = 0; i < options.size(); i++) {
        std::string marker = (static_cast<int>(i) == default_idx) ? "*" : " ";
        std::cout << theme::color::BROWN << "      " << marker << " "
                  << (i + 1) << theme::color::RESET << "  "
                  << options[i].first;
        if (!options[i].second.empty()) {
            std::cout << theme::dim(" — " + options[i].second);
        }
        std::cout << "\n";
    }
    std::cout << theme::color::BROWN << "    Choice [" << (default_idx + 1) << "]: "
              << theme::color::RESET;
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer) || answer.empty()) return default_idx;
    try {
        int n = std::stoi(answer);
        if (n >= 1 && n <= static_cast<int>(options.size())) return n - 1;
    } catch (...) {}
    return default_idx;
}

void TCCPCLI::run_register(const std::string& path_arg) {
    namespace fs = std::filesystem;

    // Resolve target directory
    fs::path target = path_arg.empty() ? fs::current_path() : fs::path(path_arg);
    if (target.is_relative()) {
        target = fs::current_path() / target;
    }
    target = fs::canonical(target);

    if (!fs::is_directory(target)) {
        std::cout << theme::fail("Not a directory: " + target.string());
        return;
    }

    // Check for existing tccp.yaml
    fs::path config_path = target / "tccp.yaml";
    if (fs::exists(config_path)) {
        std::cout << theme::fail("tccp.yaml already exists in " + target.string());
        std::cout << theme::step("Edit it directly or delete it first.");
        return;
    }

    // Check for .gitignore (or .tccpignore)
    bool has_tccpignore = fs::exists(target / ".tccpignore");
    bool has_gitignore = fs::exists(target / ".gitignore");
    if (!has_tccpignore && !has_gitignore) {
        std::cout << theme::fail("No .gitignore found in " + target.string());
        std::cout << theme::step("Create a .gitignore before registering so tccp knows what to sync.");
        std::cout << theme::step("At minimum, add patterns for large files you don't want uploaded.");
        return;
    }

    std::string dirname = target.filename().string();

    std::cout << theme::banner();
    std::cout << theme::section("Register Project");
    std::cout << theme::kv("Directory", target.string());

    // ── 1. Project type ────────────────────────────────────

    std::vector<std::pair<std::string, std::string>> type_options = {
        {"python", "PyTorch, TensorFlow, general Python scripts"},
        {"conda",  "Conda environment with environment.yml"},
    };
    int type_idx = prompt_choice("Project type:", type_options, 0);
    std::string project_type = type_options[type_idx].first;

    // ── 2. Main script ─────────────────────────────────────

    // Try to find a sensible default
    std::string default_script = "main.py";
    if (fs::exists(target / "train.py")) default_script = "train.py";
    else if (!fs::exists(target / "main.py") && fs::exists(target / "run.py")) default_script = "run.py";

    std::string script = prompt_line("Main script", default_script);

    // ── 3. GPU ─────────────────────────────────────────────

    std::vector<std::pair<std::string, std::string>> gpu_options = {
        {"none", "CPU only"},
        {"a100", "NVIDIA A100 (40/80 GB)"},
        {"v100", "NVIDIA V100 (32 GB)"},
        {"a10",  "NVIDIA A10 (24 GB)"},
    };
    int gpu_idx = prompt_choice("GPU:", gpu_options, 0);
    std::string gpu = gpu_options[gpu_idx].first;

    // ── 4. Output directory ────────────────────────────────

    std::string output = prompt_line("Output directory (synced back after jobs)", "");

    // ── 5. Cache directory ─────────────────────────────────

    std::string cache = prompt_line("Cache directory (best-effort, persists across jobs on same node)", "");

    // ── 6. Read-only datasets ──────────────────────────────

    std::cout << "\n" << theme::dim("    Read-only data directories (large datasets synced once, comma-separated):") << "\n";
    std::string rodata_input = prompt_line("Datasets", "");

    // Parse comma-separated rodata
    std::vector<std::string> rodata;
    if (!rodata_input.empty()) {
        std::istringstream iss(rodata_input);
        std::string item;
        while (std::getline(iss, item, ',')) {
            // Trim whitespace
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty()) rodata.push_back(item);
        }
    }

    // ── Build YAML ─────────────────────────────────────────

    std::ostringstream yaml;
    yaml << "# Generated by tccp register\n";

    // Only write script if not main.py (since that's the default)
    if (script != "main.py") {
        yaml << "script: " << script << "\n";
    }

    if (project_type != "python") {
        yaml << "type: " << project_type << "\n";
    }

    if (gpu != "none") {
        yaml << "gpu: " << gpu << "\n";
    }

    if (!output.empty()) {
        yaml << "output: " << output << "\n";
    }

    if (!cache.empty()) {
        yaml << "cache: " << cache << "\n";
    }

    if (!rodata.empty()) {
        yaml << "rodata:\n";
        for (const auto& r : rodata) {
            yaml << "  - " << r << "\n";
        }
    }

    // ── Write tccp.yaml ────────────────────────────────────

    std::string content = yaml.str();

    std::cout << theme::divider();
    std::cout << theme::section("tccp.yaml");
    // Print preview
    std::istringstream preview(content);
    std::string line;
    while (std::getline(preview, line)) {
        std::cout << theme::dim("    " + line) << "\n";
    }

    std::cout << "\n";
    std::cout << theme::color::BROWN << "    Write to " << config_path.string() << "? (Y/n) "
              << theme::color::RESET;
    std::cout.flush();

    std::string confirm;
    std::getline(std::cin, confirm);
    if (!confirm.empty() && confirm[0] != 'y' && confirm[0] != 'Y') {
        std::cout << theme::dim("    Aborted.") << "\n\n";
        return;
    }

    {
        std::ofstream f(config_path);
        f << content;
    }
    std::cout << theme::green("    + ") << "tccp.yaml" << "\n";

    // ── Next steps ─────────────────────────────────────────

    std::cout << theme::divider();
    std::cout << theme::section("Next Steps");
    std::cout << "    " << theme::white("1.") << " Review and edit tccp.yaml as needed\n";
    std::cout << "    " << theme::white("2.") << " Run " << theme::blue("tccp") << " to connect and start working\n";
    std::cout << "\n";
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

// ── Manual pages ───────────────────────────────────────

static const char* MANUAL_OVERVIEW = R"(
TCCP - Tufts Cluster Command Prompt
====================================

Run your code on the Tufts HPC cluster without thinking about the cluster.
Write a script, point tccp at it, and get results back.

QUICK START
-----------

  1. tccp setup          Store your Tufts credentials (once)
  2. tccp new python     Scaffold a project, or write tccp.yaml by hand
  3. tccp                Connect and enter the interactive prompt

HOW IT WORKS
------------

  your laptop                              cluster
  +-------------+      tccp connect       +----------------+
  | tccp.yaml   | ----------------------> | your code runs |
  | train.py    |     files synced        | deps installed |
  | data/       |     deps installed      | GPU allocated  |
  +-------------+     job launched        +-------+--------+
                                                  |
                  <-------------------------------+
                    output synced back when done

  You stay in the tccp prompt. Your job runs on the cluster. When it
  finishes, output is automatically downloaded to your machine.

  If you disconnect (close laptop, lose wifi), your job keeps running.
  Reconnect with 'tccp' and pick up where you left off.

COMMANDS
--------

  run [job]         Run a job (default: "main" or the only defined job)
  view [job]        Watch a running job's live output
  jobs              List all tracked jobs and their status
  cancel <job>      Cancel a running job
  return <job>      Download output from a completed job
  shell             Open a shell on the cluster
  exec <cmd>        Run a one-off command on the cluster
  allocs            Show your active compute allocations
  dealloc [id]      Release a compute allocation
  gpus              Show available GPUs on the cluster
  help              List all commands

KEYBOARD SHORTCUTS (while viewing a job)
-----------------------------------------

  Ctrl+C            Cancel job (during setup) or detach (while running)
  Ctrl+\            Detach and return to prompt
  ESC ESC           Emergency detach

PROJECT CONFIG (tccp.yaml)
--------------------------

  Minimal:

    script: train.py

  With a GPU:

    script: train.py
    gpu: a100

  Full form:

    name: myproject
    type: python-pytorch
    script: train.py
    gpu: a100
    output: output
    cache: .model_cache
    rodata:
      - data
    env_file: .env
    jobs:
      train:
        script: train.py
        args: --epochs 100
        time: "8:00:00"
      eval: eval.py

  name       Project name (default: directory name)
  type       "python" or "python-pytorch" (default: python)
  script     Main script to run (default: main.py)
  gpu        Request a GPU: a100, v100, a10
  output     Directory downloaded back after each job
  cache      Directory that persists across jobs (model weights, etc.)
  rodata     Read-only data directories (uploaded once, not every run)
  env_file   Dotfile to upload (e.g. .env — not blocked by .gitignore)
  jobs       Named jobs (shorthand: "train: train.py" or full config)

  An empty tccp.yaml runs main.py with no GPU. The project name
  comes from the directory name.

FILE SYNC
---------

  tccp respects your .gitignore when uploading files. If you need
  different rules for the cluster, create a .tccpignore instead.
  Common large files (.git, __pycache__, .venv, etc.) are always
  skipped automatically.

GETTING STARTED
---------------

  tccp setup              Store your Tufts credentials
  tccp new <template>     Scaffold a project (python, qwen)
  tccp register [path]    Create tccp.yaml for an existing project
  tccp manual <topic>     Detailed guide (python, python-pytorch)
)";

static const char* MANUAL_PYTHON = R"(
TCCP MANUAL: Python Projects
==============================

For CPU-based Python work: data processing, scripting, CPU training.
Uses Python 3.11. No GPU.

GETTING STARTED
---------------

  mkdir myproject && cd myproject

  # Option A: scaffold from template
  tccp new python

  # Option B: create tccp.yaml for an existing project
  tccp register

  # Option C: by hand
  echo "script: main.py" > tccp.yaml

  You need a .gitignore so tccp knows what files to upload.

PROJECT LAYOUT
--------------

  myproject/
    tccp.yaml
    main.py               your script
    requirements.txt      pip dependencies (optional)
    .gitignore
    data/                 input data (optional)
    output/               results (optional, downloaded after job)

EXAMPLES
--------

  Simplest case (runs main.py):

    script: main.py

  With output and data:

    script: process.py
    output: results
    rodata:
      - data

  Multiple jobs:

    jobs:
      preprocess: preprocess.py
      train: train.py
      eval: eval.py

DEPENDENCIES
------------

  Put your pip dependencies in requirements.txt. They are installed
  automatically before your script runs. tccp caches the install by
  hash — if requirements.txt hasn't changed, it skips installation.

  Only the Python standard library is available by default. Everything
  else must go in requirements.txt.

WORKFLOW
--------

  $ tccp                          connect to the cluster
  tccp:myproject@cluster> run     submit the job
                                  (watch live output, or Ctrl+\ to detach)
  tccp:myproject@cluster> jobs    check status
  tccp:myproject@cluster> quit    disconnect (job keeps running)

  $ tccp                          reconnect later
  tccp:myproject@cluster> return  download output

TIPS
----

  - Edit requirements.txt to force a dependency reinstall next run.
  - Set "output" in tccp.yaml to auto-download results when jobs finish.
  - Use "rodata" for large input data — it's uploaded once per allocation,
    not every time you run.
)";

static const char* MANUAL_PYTHON_PYTORCH = R"(
TCCP MANUAL: Python + PyTorch Projects
========================================

For GPU training and inference. Comes with PyTorch 2.6, CUDA 12.4,
and cuDNN 9 pre-installed. Just add "gpu: a100" to your config.

GETTING STARTED
---------------

  mkdir myproject && cd myproject

  # Option A: scaffold a GPU project
  tccp new qwen

  # Option B: create tccp.yaml for an existing project
  tccp register

  # Option C: by hand
  echo -e "type: python-pytorch\nscript: train.py\ngpu: a100" > tccp.yaml

  You need a .gitignore so tccp knows what files to upload.

PROJECT LAYOUT
--------------

  myproject/
    tccp.yaml
    train.py              your training script
    requirements.txt      extra deps (torch is already provided)
    .gitignore
    .env                  secrets like HF_TOKEN (optional)
    data/                 datasets (optional)
    output/               results (optional, downloaded after job)

EXAMPLES
--------

  Minimal GPU training:

    type: python-pytorch
    script: train.py
    gpu: a100

  Full project:

    type: python-pytorch
    script: train.py
    gpu: a100
    output: checkpoints
    cache: .hf_cache
    rodata:
      - data
    env_file: .env
    jobs:
      train:
        script: train.py
        args: --epochs 50 --lr 0.001
        time: "8:00:00"

  Multiple jobs:

    type: python-pytorch
    gpu: a100
    output: results
    jobs:
      train: train.py
      eval: eval.py

GPU OPTIONS
-----------

  gpu: a100        NVIDIA A100 (40 or 80 GB, auto-selected)
  gpu: a100-80gb   Explicit 80 GB variant
  gpu: a100-40gb   Explicit 40 GB variant
  gpu: v100        NVIDIA V100 (32 GB)
  gpu: a10         NVIDIA A10 (24 GB)

  For multiple GPUs, use the full slurm block:

    slurm:
      gpu_type: a100
      gpu_count: 2
      memory: 64G
      cpus_per_task: 8

DEPENDENCIES
------------

  PyTorch, torchvision, torchaudio, and CUDA are already available.
  You do NOT need to install them. If they appear in your
  requirements.txt, tccp automatically skips them to avoid conflicts.

  Put any additional dependencies (transformers, numpy, etc.) in
  requirements.txt. They are installed automatically before your
  script runs, and cached by hash so unchanged files skip install.

  torch.cuda.is_available() will return True. Use it normally.

HUGGING FACE MODELS
-------------------

  To use gated models (Llama, Qwen, etc.):

    1. Create a .env file:       HF_TOKEN=hf_xxxxx
    2. Add to tccp.yaml:         env_file: .env
    3. Add a cache directory:    cache: .hf_cache

  The cache persists across jobs, so model weights download once.

WORKFLOW
--------

  $ tccp                          connect to the cluster
  tccp:myproject@cluster> run     submit the job
                                  (watch live output, or Ctrl+\ to detach)
  tccp:myproject@cluster> jobs    check status
  tccp:myproject@cluster> quit    disconnect (job keeps running)

  $ tccp                          reconnect later
  tccp:myproject@cluster> return  download output

  The first run takes a few extra minutes while the environment is
  prepared. Subsequent runs start much faster.

TIPS
----

  - Don't install torch via requirements.txt. It's already there and
    tccp will skip it automatically, but you'll save time by not
    listing it at all.
  - Use "cache" for model weights so they aren't re-downloaded.
  - Use "rodata" for large datasets — uploaded once, not every run.
  - Set "output" to auto-download results when jobs finish.
  - For multi-GPU, use the slurm block instead of the gpu shorthand.
)";

void TCCPCLI::run_manual(const std::string& topic) {
    if (topic.empty()) {
        std::cout << MANUAL_OVERVIEW;
    } else if (topic == "python") {
        std::cout << MANUAL_PYTHON;
    } else if (topic == "python-pytorch") {
        std::cout << MANUAL_PYTHON_PYTORCH;
    } else {
        std::cout << theme::fail("Unknown manual topic: " + topic);
        std::cout << theme::step("Available topics: python, python-pytorch");
        std::cout << theme::step("Run 'tccp manual' for the general overview.");
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
