#include "test_fixture.hpp"

TEST_F(ClusterTest, LocalStateMatchesSlurmAfterOperations) {
    connect();

    // Allocate
    auto* am = service_->alloc_manager();
    auto profile = am->resolve_profile("quick");
    auto r1 = am->allocate(profile, nullptr);
    ASSERT_TRUE(r1.is_ok());

    // Verify every local allocation exists in SLURM
    auto state = read_state_file();
    for (const auto& a : state.allocations) {
        EXPECT_TRUE(slurm_alloc_exists(a.slurm_id))
            << "Local allocation " << a.slurm_id << " not in SLURM";
    }

    // Verify no test-project SLURM allocations exist that aren't tracked locally
    auto squeue = service_->exec_remote(
        "squeue -u $USER -h -o '%i %j' 2>/dev/null | grep '" + project_name_ + "'");
    if (squeue.exit_code == 0 && !squeue.stdout_data.empty()) {
        std::istringstream iss(squeue.stdout_data);
        std::string line;
        while (std::getline(iss, line)) {
            // Extract SLURM ID (first field)
            auto space = line.find(' ');
            if (space == std::string::npos) continue;
            std::string sid = line.substr(0, space);

            bool tracked = false;
            for (const auto& a : state.allocations) {
                if (a.slurm_id == sid) { tracked = true; break; }
            }
            EXPECT_TRUE(tracked)
                << "SLURM alloc " << sid << " exists but not tracked locally";
        }
    }

    // Deallocate and verify consistency again
    service_->deallocate(r1.value.slurm_id, nullptr);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    state = read_state_file();
    for (const auto& a : state.allocations) {
        EXPECT_TRUE(slurm_alloc_exists(a.slurm_id))
            << "Post-dealloc: local allocation " << a.slurm_id << " not in SLURM";
    }
}
