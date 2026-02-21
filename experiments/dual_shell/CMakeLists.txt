add_executable(dual_shell_test main.cpp)

target_link_libraries(dual_shell_test PRIVATE
    tccp_ssh
    tccp_managers
    tccp_core
    tccp_platform
    tccp_environments
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    fmt::fmt
)

target_include_directories(dual_shell_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
