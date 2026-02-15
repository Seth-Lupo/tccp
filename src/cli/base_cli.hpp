#pragma once

#include <string>
#include <map>
#include <memory>
#include <functional>
#include <managers/tccp_service.hpp>

class BaseCLI {
public:
    BaseCLI();
    virtual ~BaseCLI() = default;

    using CommandHandler = std::function<void(BaseCLI&, const std::string&)>;

    void add_command(const std::string& name,
                    CommandHandler handler,
                    const std::string& help);

    bool require_config();
    bool require_connection();

    void execute_command(const std::string& command, const std::string& args = "");
    void print_help() const;

    // The headless service (owns config, connection, all managers)
    TccpService service;

    // Set to true when user requests exit (quit/exit command)
    bool quit_requested_ = false;

    // Returns the prompt string for readline
    std::string get_prompt_string() const;

protected:
    std::map<std::string, std::pair<CommandHandler, std::string>> commands_;
};
