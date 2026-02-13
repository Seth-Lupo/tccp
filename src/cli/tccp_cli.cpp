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
#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/directory_structure.hpp>
#include <readline/readline.h>
#include <readline/history.h>

// Check if SLURM is responding (exit code 0 = healthy)
static bool check_slurm_health(ClusterConnection& cluster) {
    auto result = cluster.login().run("sinfo -h 2>&1");
    return result.exit_code == 0;
}

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

    add_command("clear", [](BaseCLI& cli, const std::string& arg) {
        std::cout << "\033[2J\033[H" << std::flush;
    }, "Clear the screen");

    register_connection_commands(*this);
    register_jobs_commands(*this);
    register_shell_commands(*this);
    register_setup_commands(*this);
    register_credentials_commands(*this);

#ifdef TCCP_DEV
    add_command("debug", [](BaseCLI& cli, const std::string& arg) {
        if (arg == "clean-state") {
            if (!cli.state_store) {
                std::cout << theme::fail("No project loaded.");
                return;
            }
            ProjectState empty;
            cli.state_store->save(empty);
            // Reset in-memory state without destroying managers
            // (background init threads hold references to the managers)
            if (cli.allocs) {
                cli.allocs->state() = empty;
            }
            if (cli.jobs) {
                cli.jobs->clear_tracked();
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
        std::cout << theme::dim("    " + msg) << "\n";
    };

    auto result = cluster->connect(callback);
    if (result.failed()) {
        std::cout << theme::fail("Connection failed: " + result.stderr_data);
        std::cout << theme::dim("    If you're off campus, make sure the Tufts VPN is connected.") << "\n\n";
        cluster.reset();
        return;
    }
    std::cout << theme::check("Connection is HEALTHY");

    // Initialize managers
    init_managers();

    // Check SLURM health before starting REPL
    std::cout << theme::section("Checking SLURM");
    if (!check_slurm_health(*cluster)) {
        std::cout << theme::fail("SLURM is not responding.");
        std::cout << theme::dim("    Run 'exec sinfo' to test cluster connectivity.") << "\n";
        std::cout << theme::dim("    The cluster may be down or undergoing maintenance.") << "\n";
        std::cout << theme::dim("    Check for maintenance announcements from Tufts HPC.") << "\n\n";
        std::cout << theme::dim("    Try again later when the cluster is healthy.") << "\n\n";
        cluster->disconnect();
        cluster.reset();
        clear_managers();
        return;
    }
    std::cout << theme::check("SLURM controller is UP");
    std::cout << "\n";

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

        // Check SLURM health before each command
        if (!check_slurm_health(*cluster)) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("SLURM is not responding.");
            std::cout << theme::dim("    The cluster has become unavailable.") << "\n";
            std::cout << theme::dim("    Run 'exec sinfo' to test connectivity.") << "\n\n";
            std::cout << theme::dim("    Exiting. Run 'tccp' when the cluster is healthy.") << "\n\n";
            break;
        }

        // Poll for completed jobs (throttled to every 30s)
        auto now = std::chrono::steady_clock::now();
        if (jobs && (now - last_poll_time_) >= std::chrono::seconds(30)) {
            last_poll_time_ = now;
            jobs->poll([this](const TrackedJob& tj) {
                std::cout << "\n" << theme::dim(
                    "Job '" + tj.job_name + "' (" + tj.slurm_id + ") completed.") << "\n";

                // Auto-return output
                auto cb = [](const std::string& msg) {
                    std::cout << theme::dim(msg) << "\n";
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

        // Check SLURM health after each command
        if (cluster && !check_slurm_health(*cluster)) {
            std::cout << "\n" << theme::divider();
            std::cout << theme::fail("SLURM stopped responding.");
            std::cout << theme::dim("    The cluster has become unavailable.") << "\n";
            std::cout << theme::dim("    Run 'exec sinfo' to test connectivity.") << "\n\n";
            std::cout << theme::dim("    Exiting. Run 'tccp' when the cluster is healthy.") << "\n\n";
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

void TCCPCLI::run_new(const std::string& template_name) {
    if (template_name != "python" && template_name != "qwen") {
        std::cout << theme::fail("Unknown template: " + template_name);
        std::cout << theme::step("Available templates: python, qwen");
        return;
    }

    auto cwd = std::filesystem::current_path();
    std::string dirname = cwd.filename().string();

    if (template_name == "qwen") {
        // ── Qwen 3 4B chat project ──────────────────────────────
        std::cout << theme::banner();
        std::cout << theme::section("New Qwen Chat Project");
        std::cout << theme::kv("Directory", cwd.string());
        std::cout << theme::kv("Model", "Qwen/Qwen3-4B");
        std::cout << theme::kv("Type", "python-pytorch (GPU)");
        std::cout << "\n  " << theme::dim("Files") << "\n";

        // tccp.yaml — python-pytorch type with GPU
        std::string yaml =
            "name: " + dirname + "\n"
            "type: python-pytorch\n"
            "\n"
            "env_file: .env\n"
            "\n"
            "output: ./output\n"
            "\n"
            "cache: ./cache\n"
            "\n"
            "jobs:\n"
            "  chat:\n"
            "    script: chat.py\n"
            "    args: \"\"\n"
            "    time: \"2:00:00\"\n"
            "    slurm:\n"
            "      gpu_type: a100\n"
            "      gpu_count: 1\n"
            "      memory: \"32G\"\n"
            "      cpus_per_task: 4\n";
        write_if_missing(cwd / "tccp.yaml", yaml);

        // chat.py — interactive chat with Qwen3-4B-Instruct
        std::string chat_py = R"PY(import os
import torch
from dotenv import load_dotenv
from transformers import AutoModelForCausalLM, AutoTokenizer

load_dotenv()

MODEL_ID = "Qwen/Qwen3-4B"
HF_TOKEN = os.environ.get("HF_TOKEN", "").strip()

if not HF_TOKEN:
    print("ERROR: HF_TOKEN not set. Add your Hugging Face token to .env:")
    print('  HF_TOKEN=hf_...')
    raise SystemExit(1)

print(f"Loading {MODEL_ID}...")
print(f"  Device: cuda ({torch.cuda.get_device_name(0)})")
print(f"  PyTorch: {torch.__version__}")
print()

tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, token=HF_TOKEN)
model = AutoModelForCausalLM.from_pretrained(
    MODEL_ID,
    torch_dtype=torch.bfloat16,
    device_map="auto",
    token=HF_TOKEN,
)

print("Model loaded. Type 'quit' to exit, 'clear' to reset conversation.")
print()

messages = []

while True:
    try:
        user_input = input("You: ")
    except (EOFError, KeyboardInterrupt):
        print("\nBye!")
        break

    if not user_input.strip():
        continue
    if user_input.strip().lower() == "quit":
        print("Bye!")
        break
    if user_input.strip().lower() == "clear":
        messages = []
        print("-- conversation cleared --")
        print()
        continue

    messages.append({"role": "user", "content": user_input})

    text = tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=True,
        enable_thinking=False,
    )
    inputs = tokenizer(text, return_tensors="pt").to(model.device)

    with torch.no_grad():
        output_ids = model.generate(
            **inputs,
            max_new_tokens=2048,
            do_sample=True,
            temperature=0.7,
            top_p=0.9,
            top_k=20,
        )

    # Decode only the new tokens (skip the prompt)
    new_tokens = output_ids[0][inputs["input_ids"].shape[1]:]
    response = tokenizer.decode(new_tokens, skip_special_tokens=True)

    print(f"\nQwen: {response}\n")
    messages.append({"role": "assistant", "content": response})
)PY";
        write_if_missing(cwd / "chat.py", chat_py);

        // Create output directory
        std::filesystem::create_directories(cwd / "output");

        // requirements.txt — transformers + friends (torch comes from container)
        std::string requirements =
            "transformers\n"
            "accelerate\n"
            "safetensors\n"
            "sentencepiece\n"
            "python-dotenv\n";
        write_if_missing(cwd / "requirements.txt", requirements);

        // .env — Hugging Face token (gitignored, synced via env_file)
        std::string env =
            "# Hugging Face access token (https://huggingface.co/settings/tokens)\n"
            "HF_TOKEN=\n";
        write_if_missing(cwd / ".env", env);

        // .gitignore
        std::string gitignore =
            "__pycache__/\n"
            "*.pyc\n"
            ".venv/\n"
            "*.egg-info/\n"
            ".env\n"
            "output/\n"
            "tccp-output/\n"
            "cache/\n";
        write_if_missing(cwd / ".gitignore", gitignore);

        std::cout << theme::divider();
        std::cout << theme::section("Next Steps");
        std::cout << "    " << theme::white("1.") << " Add your Hugging Face token to "
                  << theme::bold(".env") << "\n";
        std::cout << "    " << theme::white("2.") << " Run " << theme::bold("tccp")
                  << " to connect\n";
        std::cout << "    " << theme::white("3.") << " Run " << theme::bold("run chat")
                  << " to start chatting\n\n";
        return;
    }

    // ── Python template (original) ──────────────────────────────
    std::cout << theme::banner();
    std::cout << theme::section("New Python Project");
    std::cout << theme::kv("Directory", cwd.string());
    std::cout << "\n  " << theme::dim("Files") << "\n";

    // tccp.yaml
    std::string yaml =
        "name: " + dirname + "\n"
        "type: python\n"
        "\n"
        "rodata:\n"
        "  - ./data\n"
        "\n"
        "output: ./output\n"
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
        "data = np.loadtxt(\"data/input.csv\", delimiter=\",\")\n"
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

    // Create output directory
    std::filesystem::create_directories(cwd / "output");

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
        "output/\n"
        "tccp-output/\n";
    write_if_missing(cwd / ".gitignore", gitignore);

    std::cout << theme::divider();
    std::cout << theme::section("Next Steps");
    std::cout << "    " << theme::white("1.") << " Run " << theme::bold("tccp")
              << " to connect\n";
    std::cout << "    " << theme::white("2.") << " Run " << theme::bold("run main")
              << " to submit your job\n\n";
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
