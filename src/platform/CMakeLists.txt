add_library(tccp_platform
    platform.cpp
    terminal.cpp
    socket_util.cpp
    process.cpp
    archive.cpp
    singleton.cpp
)

target_include_directories(tccp_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(LibArchive REQUIRED)
target_link_libraries(tccp_platform PRIVATE LibArchive::LibArchive)

if(WIN32)
    target_compile_definitions(tccp_platform PRIVATE WIN32_LEAN_AND_MEAN)
    target_link_libraries(tccp_platform PRIVATE ws2_32)
endif()
