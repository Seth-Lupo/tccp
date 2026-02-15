add_library(tccp_environments
    environment.cpp
    templates.cpp
    python_template.cpp
    qwen_template.cpp
)

target_include_directories(tccp_environments PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
