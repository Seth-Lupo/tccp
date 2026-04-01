#include <gtest/gtest.h>
#include <core/config.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / ("tccp-config-test-" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override {
        if (fs::exists(tmp_dir_)) {
            fs::remove_all(tmp_dir_);
        }
    }

    void write_yaml(const std::string& content) {
        std::ofstream f((tmp_dir_ / "tccp.yaml").string());
        f << content;
    }

    Result<Config> load() {
        return Config::load_project(tmp_dir_);
    }
};

// ── Minimal valid config ─────────────────────────────────────

TEST_F(ConfigTest, MinimalValidConfig) {
    write_yaml(R"(
name: my-project
type: python
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().name, "my-project");
    EXPECT_EQ(r.value.project().type, "python");
    EXPECT_EQ(r.value.project().jobs.size(), 1u);
    EXPECT_EQ(r.value.project().jobs.at("train").script, "train.py");
}

// ── Missing name infers from directory ───────────────────────

TEST_F(ConfigTest, MissingNameInfersFromDir) {
    write_yaml(R"(
type: python
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    // Name is inferred from tmp_dir_ filename
    EXPECT_EQ(r.value.project().name, tmp_dir_.filename().string());
}

// ── GPU shorthand: "none" ────────────────────────────────────

TEST_F(ConfigTest, GpuNone) {
    write_yaml(R"(
name: test
type: python
gpu: none
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    ASSERT_TRUE(r.value.project().slurm.has_value());
    EXPECT_EQ(r.value.project().slurm->gpu_type, "");
    EXPECT_EQ(r.value.project().slurm->gpu_count, 0);
}

// ── GPU shorthand: "a100:2" ─────────────────────────────────

TEST_F(ConfigTest, GpuWithCount) {
    write_yaml(R"(
name: test
type: python
gpu: "a100:2"
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    ASSERT_TRUE(r.value.project().slurm.has_value());
    // a100 may be resolved to a variant, but count should be 2
    EXPECT_EQ(r.value.project().slurm->gpu_count, 2);
    EXPECT_FALSE(r.value.project().slurm->gpu_type.empty());
}

// ── Empty jobs map doesn't crash ─────────────────────────────

TEST_F(ConfigTest, EmptyJobsMap) {
    write_yaml(R"(
name: test
type: python
jobs:
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    // When jobs is empty/null, a "main" job is auto-created
    EXPECT_EQ(r.value.project().jobs.size(), 1u);
    EXPECT_EQ(r.value.project().jobs.count("main"), 1u);
}

// ── Job with package instead of script ───────────────────────

TEST_F(ConfigTest, JobWithPackage) {
    write_yaml(R"(
name: test
type: python
jobs:
  serve:
    package: myapp.server
    args: "--port 8080"
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    auto& job = r.value.project().jobs.at("serve");
    EXPECT_EQ(job.package, "myapp.server");
    EXPECT_EQ(job.args, "--port 8080");
    EXPECT_EQ(job.script, "");
}

// ── Bare string job shorthand ────────────────────────────────

TEST_F(ConfigTest, BareStringJobShorthand) {
    write_yaml(R"(
name: test
type: python
jobs:
  train: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().jobs.at("train").script, "train.py");
}

// ── Per-job SLURM overrides ─────────────────────────────────

TEST_F(ConfigTest, PerJobSlurmOverrides) {
    write_yaml(R"(
name: test
type: python
jobs:
  train:
    script: train.py
    slurm:
      partition: gpu
      memory: 16G
      cpus_per_task: 4
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    auto& job = r.value.project().jobs.at("train");
    ASSERT_TRUE(job.slurm.has_value());
    EXPECT_EQ(job.slurm->partition, "gpu");
    EXPECT_EQ(job.slurm->memory, "16G");
    EXPECT_EQ(job.slurm->cpus_per_task, 4);
}

// ── Port forwarding ──────────────────────────────────────────

TEST_F(ConfigTest, PortForwarding) {
    write_yaml(R"(
name: test
type: python
jobs:
  serve:
    script: serve.py
    ports: [8080, 8081]
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    auto& job = r.value.project().jobs.at("serve");
    ASSERT_EQ(job.ports.size(), 2u);
    EXPECT_EQ(job.ports[0], 8080);
    EXPECT_EQ(job.ports[1], 8081);
}

// ── Top-level port applies to jobs without own ports ─────────

TEST_F(ConfigTest, TopLevelPortInherited) {
    write_yaml(R"(
name: test
type: python
port: 6006
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    auto& job = r.value.project().jobs.at("train");
    ASSERT_EQ(job.ports.size(), 1u);
    EXPECT_EQ(job.ports[0], 6006);
}

// ── No config file → error ──────────────────────────────────

TEST_F(ConfigTest, MissingConfigFileErrors) {
    // Don't write any yaml
    auto r = Config::load_project(tmp_dir_);
    EXPECT_TRUE(r.is_err());
}

// ── .env config tests ───────────────────────────────────────

TEST_F(ConfigTest, DefaultEnvIsEnvFile) {
    write_yaml(R"(
name: test
type: python
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().env, ".env");
}

TEST_F(ConfigTest, CustomEnvPreserved) {
    write_yaml(R"(
name: test
type: python
env: "config/.env.prod"
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().env, "config/.env.prod");
}

TEST_F(ConfigTest, LegacyEnvFileKeyWorks) {
    write_yaml(R"(
name: test
type: python
env_file: "my.env"
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().env, "my.env");
}

TEST_F(ConfigTest, EnvironmentMapRoundTrip) {
    write_yaml(R"(
name: test
type: python
environment:
  FOO: bar
  NUM: "42"
jobs:
  train:
    script: train.py
)");
    auto r = load();
    ASSERT_TRUE(r.is_ok()) << r.error;
    EXPECT_EQ(r.value.project().environment.size(), 2u);
    EXPECT_EQ(r.value.project().environment.at("FOO"), "bar");
    EXPECT_EQ(r.value.project().environment.at("NUM"), "42");
}
