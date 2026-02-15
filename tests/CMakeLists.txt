find_package(GTest CONFIG REQUIRED)

add_executable(tccp_tests
    test_time_utils.cpp
    test_resource_spec.cpp
    test_gitignore.cpp
    test_gpu_selection.cpp
)

target_link_libraries(tccp_tests PRIVATE
    libtccp
    GTest::gtest
    GTest::gtest_main
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    fmt::fmt
)

include(GoogleTest)
gtest_discover_tests(tccp_tests)
