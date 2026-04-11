cmake_minimum_required(VERSION 3.20)
project(tccp VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(yaml-cpp CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_compile_definitions(TCCP_VERSION="${PROJECT_VERSION}")

add_executable(tccp
    src/main.cpp
    src/config.cpp
    src/ssh.cpp
    src/sync.cpp
    src/session.cpp
    src/state.cpp)

target_include_directories(tccp PRIVATE src)
target_link_libraries(tccp PRIVATE
    yaml-cpp::yaml-cpp
    CLI11::CLI11
    fmt::fmt
    Threads::Threads)
