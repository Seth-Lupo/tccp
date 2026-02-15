#include "preflight.hpp"
#include <core/config.hpp>
#include <core/credentials.hpp>

std::vector<PreflightIssue> check_credentials() {
    std::vector<PreflightIssue> issues;
    auto& creds = CredentialManager::instance();

    auto user = creds.get("user");
    if (user.is_err() || user.value.empty()) {
        issues.push_back({"No username configured", "Run 'tccp setup'"});
    }

    auto pass = creds.get("password");
    if (pass.is_err() || pass.value.empty()) {
        issues.push_back({"No password configured", "Run 'tccp setup'"});
    }

    return issues;
}

std::vector<PreflightIssue> check_global_config() {
    std::vector<PreflightIssue> issues;

    if (!global_config_exists()) {
        issues.push_back({
            "Global config not found at " + get_global_config_path().string(),
            "Create ~/.tccp/config.yaml with dtn: and login: sections"
        });
        return issues;
    }

    auto result = Config::load_global();
    if (result.is_err()) {
        issues.push_back({"Failed to parse global config: " + result.error, "Check YAML syntax"});
        return issues;
    }

    const auto& cfg = result.value;

    if (cfg.dtn().host.empty()) {
        issues.push_back({"DTN host not configured", "Set dtn.host in ~/.tccp/config.yaml"});
    }

    if (cfg.login().host.empty()) {
        issues.push_back({"Login host not configured", "Set login.host in ~/.tccp/config.yaml"});
    }

    return issues;
}

std::vector<PreflightIssue> check_project_config() {
    std::vector<PreflightIssue> issues;

    if (!project_config_exists()) {
        issues.push_back({
            "No tccp.yaml in current directory",
            "Run 'tccp new python' or create tccp.yaml"
        });
        return issues;
    }

    auto result = Config::load_project();
    if (result.is_err()) {
        issues.push_back({"Failed to parse tccp.yaml: " + result.error, "Check YAML syntax"});
        return issues;
    }

    const auto& proj = result.value.project();

    return issues;
}

std::vector<PreflightIssue> run_preflight_checks() {
    std::vector<PreflightIssue> all;

    auto cred_issues = check_credentials();
    all.insert(all.end(), cred_issues.begin(), cred_issues.end());

    auto config_issues = check_global_config();
    all.insert(all.end(), config_issues.begin(), config_issues.end());

    auto project_issues = check_project_config();
    all.insert(all.end(), project_issues.begin(), project_issues.end());

    return all;
}
