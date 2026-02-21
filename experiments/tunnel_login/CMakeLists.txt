add_executable(tunnel_login_test main.cpp)

target_link_libraries(tunnel_login_test PRIVATE
    tccp_ssh
    tccp_managers
    tccp_core
    tccp_platform
    tccp_environments
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    fmt::fmt
)

target_include_directories(tunnel_login_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
