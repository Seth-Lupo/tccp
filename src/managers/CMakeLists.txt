add_library(tccp_managers
    job_manager.cpp
    gitignore.cpp
)

target_include_directories(tccp_managers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_managers PUBLIC fmt::fmt)
target_link_libraries(tccp_managers PRIVATE tccp_core tccp_ssh)
