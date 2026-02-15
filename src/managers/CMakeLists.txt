add_library(tccp_managers
    job_manager.cpp
    job_environment.cpp
    job_output.cpp
    job_cleanup.cpp
    gitignore.cpp
    state_store.cpp
    allocation_manager.cpp
    sync_manager.cpp
    gpu_discovery.cpp
    cache_manager.cpp
    tccp_service.cpp
)

target_include_directories(tccp_managers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_managers PUBLIC fmt::fmt yaml-cpp::yaml-cpp)
target_link_libraries(tccp_managers PRIVATE tccp_core tccp_ssh tccp_environments)
