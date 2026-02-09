add_library(tccp_util
    string_utils.cpp
    path_utils.cpp
    process.cpp
)

target_include_directories(tccp_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_util PUBLIC fmt::fmt)
