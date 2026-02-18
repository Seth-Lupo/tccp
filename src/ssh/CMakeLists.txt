add_library(tccp_ssh
    connection.cpp
    session.cpp
    auth.cpp
    expect.cpp
    cluster_connection.cpp
)

target_include_directories(tccp_ssh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_ssh PUBLIC libssh2::libssh2 fmt::fmt)
target_link_libraries(tccp_ssh PRIVATE tccp_core tccp_platform)
