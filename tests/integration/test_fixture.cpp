#include "test_fixture.hpp"
#include <core/utils.hpp>
#include <core/constants.hpp>
#include <platform/platform.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <sstream>

std::atomic<int> ClusterTest::counter_{0};

// Register global test environment for pre-suite cleanup
static auto* g_test_env = ::testing::AddGlobalTestEnvironment(new TestEnvironment());

void ClusterTest::SetUp() {
    const char* env = std::getenv("TCCP_INTEGRATION");
    if (!env || std::string(env).empty()) {
        GTEST_SKIP() << "TCCP_INTEGRATION not set, skipping cluster test";
    }

    int n = counter_++;
    project_name_ = "tccp-test-" + std::to_string(getpid()) + "-" + std::to_string(n);
    project_dir_ = fs::temp_directory_path() / project_name_;
    fs::create_directories(project_dir_);

    write_project_files();

    // Clean any leftover local state from a previous crashed run
    StateStore store(project_name_);
    if (fs::exists(store.path())) {
        fs::remove(store.path());
    }
}

void ClusterTest::TearDown() {
    if (service_ && service_->is_connected()) {
        force_cancel_all();
        clean_remote();
        service_->disconnect();
    }
    service_.reset();

    if (!project_dir_.empty() && fs::exists(project_dir_)) {
        fs::remove_all(project_dir_);
    }

    if (!project_name_.empty()) {
        StateStore store(project_name_);
        if (fs::exists(store.path())) {
            fs::remove(store.path());
        }
    }
}

void ClusterTest::write_project_files() {
    {
        std::ofstream f((project_dir_ / "tccp.yaml").string());
        f << "name: " << project_name_ << "\n"
          << "type: python\n"
          << "jobs:\n"
          << "  quick:\n"
          << "    script: test_quick.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:01:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n"
          << "  slow:\n"
          << "    script: test_slow.py\n"
          << "    time: \"0:05:00\"\n"
          << "    exp_time: \"0:03:00\"\n"
          << "    slurm:\n"
          << "      partition: batch\n"
          << "      gpu: none\n"
          << "      cpus_per_task: 1\n"
          << "      memory: 1G\n";
    }

    {
        std::ofstream f((project_dir_ / "test_quick.py").string());
        f << "print('hello from tccp test')\n"
          << "import time; time.sleep(3)\n";
    }

    {
        std::ofstream f((project_dir_ / "test_slow.py").string());
        f << "import time; time.sleep(300)\n";
    }
}

void ClusterTest::connect() {
    auto old_dir = fs::current_path();
    fs::current_path(project_dir_);
    service_ = std::make_unique<TccpService>();
    auto result = service_->connect();
    fs::current_path(old_dir);
    ASSERT_TRUE(result.is_ok()) << "connect() failed: " << result.error;

    // Clean stale SLURM allocations and remote dirs from previous runs
    force_cancel_all();
    clean_remote();
}

void ClusterTest::reconnect() {
    // Tear down current service completely (simulates disconnect)
    if (service_) {
        service_->disconnect();
        service_.reset();
    }

    // Create a fresh service — will reload state from disk
    auto old_dir = fs::current_path();
    fs::current_path(project_dir_);
    service_ = std::make_unique<TccpService>();
    auto result = service_->connect();
    fs::current_path(old_dir);
    ASSERT_TRUE(result.is_ok()) << "reconnect() failed: " << result.error;
}

void ClusterTest::wait_job_running(const std::string& job_name, int timeout_s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        service_->poll_jobs([](const TrackedJob&) {});
        auto* tj = service_->find_job_by_name(job_name);
        if (tj && tj->init_complete && !tj->completed) return;
        if (tj && tj->completed) {
            FAIL() << "Job '" << job_name << "' completed before reaching RUNNING";
        }
        if (tj && !tj->init_error.empty()) {
            FAIL() << "Job '" << job_name << "' init error: " << tj->init_error;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    FAIL() << "Timed out waiting for job '" << job_name << "' to reach RUNNING";
}

void ClusterTest::wait_job_completed(const std::string& job_name, int timeout_s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        service_->poll_jobs([](const TrackedJob&) {});
        auto* tj = service_->find_job_by_name(job_name);
        if (tj && tj->completed) return;
        if (tj && !tj->init_error.empty()) {
            FAIL() << "Job '" << job_name << "' init error: " << tj->init_error;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    FAIL() << "Timed out waiting for job '" << job_name << "' to complete";
}

bool ClusterTest::slurm_alloc_exists(const std::string& slurm_id) {
    auto result = service_->exec_remote(
        "squeue -j " + slurm_id + " -h -o '%i' 2>/dev/null");
    if (result.exit_code != 0) return false;
    std::string out = result.stdout_data;
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}

ProjectState ClusterTest::read_state_file() {
    StateStore store(project_name_);
    return store.load();
}

void ClusterTest::write_state_file(const ProjectState& state) {
    StateStore store(project_name_);
    store.save(state);
}

void ClusterTest::force_cancel_all() {
    if (!service_ || !service_->is_connected()) return;

    auto result = service_->exec_remote(
        "squeue -u $USER -h -o '%i %j' 2>/dev/null | grep '" + project_name_ +
        "' | awk '{print $1}'");
    if (result.exit_code == 0 && !result.stdout_data.empty()) {
        std::istringstream iss(result.stdout_data);
        std::string id;
        while (std::getline(iss, id)) {
            while (!id.empty() && (id.back() == '\n' || id.back() == ' '))
                id.pop_back();
            if (!id.empty()) {
                service_->exec_remote("scancel " + id + " 2>/dev/null");
            }
        }
    }

    if (service_->alloc_manager()) {
        service_->deallocate("", nullptr);
    }
}

void ClusterTest::clean_remote() {
    if (!service_ || !service_->is_connected()) return;

    service_->exec_remote(
        "rm -rf ~/tccp/projects/" + project_name_ + " 2>/dev/null");
    service_->exec_remote(
        "rm -rf /tmp/$USER/" + project_name_ + " 2>/dev/null");
}

SSHResult ClusterTest::run_on_dtn(const std::string& command, int timeout_secs) {
    return service_->cluster()->dtn().run(command, timeout_secs);
}

SSHResult ClusterTest::run_on_compute(const std::string& node, const std::string& command,
                                       int timeout_secs) {
    std::string hop = "ssh " + std::string(SSH_OPTS) + " " + node + " '" + command + "'";
    return service_->cluster()->dtn().run(hop, timeout_secs);
}

std::string ClusterTest::cluster_user() {
    return get_cluster_username();
}

void ClusterTest::assert_state_consistent() {
    auto state = read_state_file();

    // 1. Every local allocation must exist in SLURM
    for (const auto& a : state.allocations) {
        EXPECT_TRUE(slurm_alloc_exists(a.slurm_id))
            << "INCONSISTENT: local allocation " << a.slurm_id << " not in SLURM";
    }

    // 2. No allocation should have active_job_id referencing a completed/missing job
    for (const auto& a : state.allocations) {
        if (a.active_job_id.empty()) continue;
        bool job_active = false;
        for (const auto& j : state.jobs) {
            if (j.job_id == a.active_job_id && !j.completed && !j.canceled && j.init_error.empty()) {
                job_active = true;
                break;
            }
        }
        EXPECT_TRUE(job_active)
            << "INCONSISTENT: allocation " << a.slurm_id
            << " references dead/missing job '" << a.active_job_id << "'";
    }

    // 3. Every running job should reference an allocation that exists
    for (const auto& j : state.jobs) {
        if (j.completed || j.canceled || !j.init_error.empty()) continue;
        if (j.alloc_slurm_id.empty()) continue;
        bool alloc_exists = false;
        for (const auto& a : state.allocations) {
            if (a.slurm_id == j.alloc_slurm_id) {
                alloc_exists = true;
                break;
            }
        }
        EXPECT_TRUE(alloc_exists)
            << "INCONSISTENT: running job '" << j.job_id
            << "' references missing allocation " << j.alloc_slurm_id;
    }

    // 4. No orphan SLURM allocations for this project not tracked locally
    auto squeue = service_->exec_remote(
        "squeue -u $USER -h -o '%i %j' 2>/dev/null | grep '" + project_name_ + "'");
    if (squeue.exit_code == 0 && !squeue.stdout_data.empty()) {
        std::istringstream iss(squeue.stdout_data);
        std::string line;
        while (std::getline(iss, line)) {
            auto space = line.find(' ');
            if (space == std::string::npos) continue;
            std::string sid = line.substr(0, space);
            bool tracked = false;
            for (const auto& a : state.allocations) {
                if (a.slurm_id == sid) { tracked = true; break; }
            }
            EXPECT_TRUE(tracked)
                << "INCONSISTENT: SLURM alloc " << sid << " not tracked locally";
        }
    }
}

// ── TestEnvironment ──────────────────────────────────────────

void TestEnvironment::SetUp() {
    clean_all_test_projects();
}

void TestEnvironment::clean_all_test_projects() {
    // Clean all tccp-test-* state files from the isolated test state dir
    fs::path test_state_dir = fs::temp_directory_path() / ".tccp-test" / "state";
    if (fs::exists(test_state_dir)) {
        for (const auto& entry : fs::directory_iterator(test_state_dir)) {
            if (entry.path().filename().string().find("tccp-test-") == 0) {
                fs::remove(entry.path());
            }
        }
    }

    // Also clean any stale state from the real state dir (from old test runs)
    fs::path real_state_dir = platform::home_dir() / ".tccp" / "state";
    if (fs::exists(real_state_dir)) {
        for (const auto& entry : fs::directory_iterator(real_state_dir)) {
            if (entry.path().filename().string().find("tccp-test-") == 0) {
                fs::remove(entry.path());
            }
        }
    }
}
