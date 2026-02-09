add_library(tccp_core
    config.cpp
    credentials.cpp
)

target_include_directories(tccp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tccp_core PUBLIC yaml-cpp::yaml-cpp fmt::fmt)

if(TCCP_DEV)
    # Dev: file-based credentials (no keychain prompts on recompile)
    target_sources(tccp_core PRIVATE platform/credentials_file.cpp)
    target_compile_definitions(tccp_core PUBLIC TCCP_DEV=1)
elseif(APPLE)
    target_sources(tccp_core PRIVATE platform/credentials_macos.cpp)
    target_link_libraries(tccp_core PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_sources(tccp_core PRIVATE platform/credentials_win.cpp)
    target_link_libraries(tccp_core PRIVATE crypt32 advapi32)
else()
    target_sources(tccp_core PRIVATE platform/credentials_linux.cpp)
    if(LIBSECRET_FOUND)
        target_link_libraries(tccp_core PRIVATE ${LIBSECRET_LIBRARIES})
        target_include_directories(tccp_core PRIVATE ${LIBSECRET_INCLUDE_DIRS})
    endif()
endif()
