add_library(tccp_ssh
    connection.cpp
    session.cpp
    auth.cpp
    expect.cpp
    connection_factory.cpp
    login_hop_connection.cpp
    marker_protocol.cpp
    session_multiplexer.cpp
    multiplexed_connection.cpp
    multiplexed_login_hop.cpp
    multiplexed_shell.cpp
)

target_include_directories(tccp_ssh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_ssh PUBLIC libssh2::libssh2 fmt::fmt)
target_link_libraries(tccp_ssh PRIVATE tccp_core tccp_platform)
