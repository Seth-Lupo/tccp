#include "tccp_cli.hpp"
#include "theme.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <core/config.hpp>
#include <core/credentials.hpp>
#include <core/directory_structure.hpp>
#include <environments/templates.hpp>

// ── Interactive prompt helpers ──────────────────────────

static std::string prompt_line(const std::string& label, const std::string& default_val = "") {
    std::string suffix = default_val.empty() ? ": " : " [" + default_val + "]: ";
    std::cout << theme::color::BROWN << "    " << label << suffix << theme::color::RESET;
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer)) return default_val;
    if (answer.empty()) return default_val;
    return answer;
}

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

void TCCPCLI::run_register(const std::string& path_arg) {
    namespace fs = std::filesystem;

    fs::path target = path_arg.empty() ? fs::current_path() : fs::path(path_arg);
    if (target.is_relative()) {
        target = fs::current_path() / target;
    }
    target = fs::canonical(target);

    if (!fs::is_directory(target)) {
        std::cout << theme::fail("Not a directory: " + target.string());
        return;
    }

    fs::path config_path = target / "tccp.yaml";
    if (fs::exists(config_path)) {
        std::cout << theme::fail("tccp.yaml already exists in " + target.string());
        std::cout << theme::step("Edit it directly or delete it first.");
        return;
    }

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

    std::vector<std::pair<std::string, std::string>> type_options = {
        {"python", "PyTorch, TensorFlow, general Python scripts"},
        {"conda",  "Conda environment with environment.yml"},
    };
    int type_idx = prompt_choice("Project type:", type_options, 0);
    std::string project_type = type_options[type_idx].first;

    std::string default_script = "main.py";
    if (fs::exists(target / "train.py")) default_script = "train.py";
    else if (!fs::exists(target / "main.py") && fs::exists(target / "run.py")) default_script = "run.py";

    std::string script = prompt_line("Main script", default_script);

    std::vector<std::pair<std::string, std::string>> gpu_options = {
        {"none", "CPU only"},
        {"a100", "NVIDIA A100 (40/80 GB)"},
        {"v100", "NVIDIA V100 (32 GB)"},
        {"a10",  "NVIDIA A10 (24 GB)"},
    };
    int gpu_idx = prompt_choice("GPU:", gpu_options, 0);
    std::string gpu = gpu_options[gpu_idx].first;

    std::string output = prompt_line("Output directory (synced back after jobs)", "");
    std::string cache = prompt_line("Cache directory (best-effort, persists across jobs on same node)", "");

    std::cout << "\n" << theme::dim("    Read-only data directories (large datasets synced once, comma-separated):") << "\n";
    std::string rodata_input = prompt_line("Datasets", "");

    std::vector<std::string> rodata;
    if (!rodata_input.empty()) {
        std::istringstream iss(rodata_input);
        std::string item;
        while (std::getline(iss, item, ',')) {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (!item.empty()) rodata.push_back(item);
        }
    }

    std::ostringstream yaml;
    yaml << "# Generated by tccp register\n";
    if (script != "main.py") yaml << "script: " << script << "\n";
    if (project_type != "python") yaml << "type: " << project_type << "\n";
    if (gpu != "none") yaml << "gpu: " << gpu << "\n";
    if (!output.empty()) yaml << "output: " << output << "\n";
    if (!cache.empty()) yaml << "cache: " << cache << "\n";
    if (!rodata.empty()) {
        yaml << "rodata:\n";
        for (const auto& r : rodata) yaml << "  - " << r << "\n";
    }

    std::string content = yaml.str();

    std::cout << theme::divider();
    std::cout << theme::section("tccp.yaml");
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

    std::cout << theme::divider();
    std::cout << theme::section("Next Steps");
    std::cout << "    " << theme::white("1.") << " Review and edit tccp.yaml as needed\n";
    std::cout << "    " << theme::white("2.") << " Run " << theme::blue("tccp") << " to connect and start working\n";
    std::cout << "\n";
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

    for (const auto& file : tmpl->files) {
        std::string content = substitute(file.content, dirname);
        std::filesystem::path fpath = cwd / file.path;
        std::filesystem::create_directories(fpath.parent_path());
        write_if_missing(fpath, content);
    }

    for (const auto& dir : tmpl->directories) {
        std::filesystem::create_directories(cwd / dir);
    }

    std::cout << theme::divider();
    std::cout << theme::section("Next Steps");
    for (size_t i = 0; i < tmpl->next_steps.size(); i++) {
        std::cout << "    " << theme::white(std::to_string(i + 1) + ".")
                  << " " << tmpl->next_steps[i] << "\n";
    }
    std::cout << "\n";
}

void TCCPCLI::run_setup() {
    ensure_tccp_directory_structure();

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

    std::string user;
    std::cout << theme::color::BROWN << "    Tufts username: " << theme::color::RESET;
    std::cout.flush();
    std::getline(std::cin, user);

    if (user.empty()) {
        std::cout << theme::fail("Username cannot be empty.");
        return;
    }

    std::string password = read_password(
        theme::color::BROWN + "    Tufts password: " + theme::color::RESET
    );
    if (password.empty()) {
        std::cout << theme::fail("Password cannot be empty.");
        return;
    }

    std::string email;
    std::cout << theme::color::BROWN << "    Email (for job notifications): " << theme::color::RESET;
    std::cout.flush();
    std::getline(std::cin, email);

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
