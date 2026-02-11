add_library(tccp_cli
    base_cli.cpp
    tccp_cli.cpp
    preflight.cpp
    terminal_output.cpp
    terminal_ui.cpp
    job_view.cpp
    commands/setup.cpp
    commands/connection.cpp
    commands/jobs.cpp
    commands/shell.cpp
    commands/credentials.cpp
)

target_include_directories(tccp_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_cli PUBLIC CLI11::CLI11 fmt::fmt)
target_link_libraries(tccp_cli PRIVATE tccp_core tccp_ssh tccp_managers)

# Readline (libedit on macOS, GNU readline on Linux)
if(APPLE)
    target_link_libraries(tccp_cli PRIVATE "-ledit")
else()
    find_library(READLINE_LIB readline)
    if(READLINE_LIB)
        target_link_libraries(tccp_cli PRIVATE ${READLINE_LIB})
    endif()
endif()
