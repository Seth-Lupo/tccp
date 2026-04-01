#include "test_fixture.hpp"

TEST_F(ClusterTest, ConnectSucceeds) {
    connect();
    EXPECT_TRUE(service_->is_connected());
}

TEST_F(ClusterTest, CheckAlive) {
    connect();
    EXPECT_TRUE(service_->check_alive());
}

TEST_F(ClusterTest, CheckSlurmHealth) {
    connect();
    EXPECT_TRUE(service_->check_slurm_health());
}

TEST_F(ClusterTest, DisconnectAndReconnect) {
    connect();
    EXPECT_TRUE(service_->is_connected());

    service_->disconnect();
    EXPECT_FALSE(service_->is_connected());

    // Reconnect with fresh service (need to chdir again)
    connect();
    EXPECT_TRUE(service_->is_connected());
}

TEST_F(ClusterTest, ExecRemote) {
    connect();
    auto result = service_->exec_remote("echo hello");
    EXPECT_EQ(result.exit_code, 0);
    std::string out = result.stdout_data;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    EXPECT_EQ(out, "hello");
}
