cmake_minimum_required(VERSION 3.20)
set(VCPKG_INSTALL_OPTIONS "--no-print-usage")
project(tccp VERSION 0.3.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Static linking preference
option(TCCP_STATIC_BUILD "Build with static libraries" ON)
option(TCCP_DEV "Dev build (file-based credentials, no keychain prompt)" ON)

if(TCCP_STATIC_BUILD)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a .lib)
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# vcpkg toolchain must be used
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(WARNING "CMAKE_TOOLCHAIN_FILE not set. Build may fail. Set it to vcpkg toolchain.")
endif()

# Find dependencies
find_package(libssh2 CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(replxx CONFIG REQUIRED)

# Platform-specific libraries
if(APPLE AND NOT TCCP_DEV)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
elseif(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET libsecret-1)
    endif()
endif()

# Test option
option(TCCP_BUILD_TESTS "Build unit tests" OFF)

# Version define — single source of truth (from project() VERSION above)
add_compile_definitions(TCCP_VERSION="${PROJECT_VERSION}")

# Source subdirectories
add_subdirectory(src/platform)
add_subdirectory(src/core)
add_subdirectory(src/ssh)
add_subdirectory(src/environments)
add_subdirectory(src/managers)
add_subdirectory(src/cli)

# libtccp: interface library bundling all core libraries (for tests and future frontends)
add_library(libtccp INTERFACE)
target_link_libraries(libtccp INTERFACE
    tccp_platform
    tccp_core
    tccp_ssh
    tccp_environments
    tccp_managers
)

# Main executable
add_executable(tccp src/main.cpp)

target_link_libraries(tccp PRIVATE
    tccp_platform
    tccp_core
    tccp_ssh
    tccp_environments
    tccp_managers
    tccp_cli
    libssh2::libssh2
    yaml-cpp::yaml-cpp
    CLI11::CLI11
    fmt::fmt
)

# Platform-specific linking
if(APPLE AND NOT TCCP_DEV)
    target_link_libraries(tccp PRIVATE
        ${SECURITY_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
elseif(WIN32)
    target_link_libraries(tccp PRIVATE crypt32 advapi32 ws2_32)
elseif(UNIX)
    if(LIBSECRET_FOUND)
        target_link_directories(tccp PRIVATE ${LIBSECRET_LIBRARY_DIRS})
        target_include_directories(tccp PRIVATE ${LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(tccp PRIVATE ${LIBSECRET_LIBRARIES})
    endif()
    target_link_libraries(tccp PRIVATE pthread)
endif()

# Tests
if(TCCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
install(TARGETS tccp DESTINATION bin)
