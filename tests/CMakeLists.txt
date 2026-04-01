find_package(GTest CONFIG REQUIRED)

find_package(LibArchive REQUIRED)

add_executable(tccp_tests
    test_time_utils.cpp
    test_resource_spec.cpp
    test_gitignore.cpp
    test_gpu_selection.cpp
    test_state_store.cpp
    test_state_invariants.cpp
    test_allocation_logic.cpp
    test_archive.cpp
    test_config.cpp
    test_allocation_time.cpp
)

target_link_libraries(tccp_tests PRIVATE
    libtccp
    GTest::gtest
    GTest::gtest_main
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    fmt::fmt
    LibArchive::LibArchive
)

include(GoogleTest)
gtest_discover_tests(tccp_tests)

# Integration tests (requires cluster, gated by TCCP_INTEGRATION env var)
add_executable(tccp_integration_tests
    integration/test_fixture.cpp
    integration/test_int_connect.cpp
    integration/test_int_allocation.cpp
    integration/test_int_alloc_reuse.cpp
    integration/test_int_alloc_edge_cases.cpp
    integration/test_int_stale_state.cpp
    integration/test_int_job_lifecycle.cpp
    integration/test_int_job_execution.cpp
    integration/test_int_cancel_edge_cases.cpp
    integration/test_int_multi_job.cpp
    integration/test_int_disconnect_reconnect.cpp
    integration/test_int_corrupt_state.cpp
    integration/test_int_cleanup.cpp
    integration/test_int_state_sync.cpp
    integration/test_int_sync.cpp
    integration/test_int_data_integrity.cpp
    integration/test_int_environment.cpp
    integration/test_int_pip.cpp
    integration/test_int_env.cpp
    integration/test_int_cache.cpp
    integration/test_int_output.cpp
    integration/test_int_cancel_timing.cpp
    integration/test_int_timeout.cpp
    integration/test_int_concurrent_jobs.cpp
    integration/test_int_large_sync.cpp
    integration/test_int_port_forward.cpp
    integration/test_int_shell_env.cpp
    integration/test_int_output_edge.cpp
    integration/test_int_crash_recovery.cpp
    integration/test_int_init_failure.cpp
)

target_link_libraries(tccp_integration_tests PRIVATE
    libtccp
    GTest::gtest
    GTest::gtest_main
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    fmt::fmt
)
