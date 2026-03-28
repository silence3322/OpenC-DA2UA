cmake_minimum_required(VERSION 3.10)
project(OpenC-DA2UA C)

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

set(CMAKE_C_STANDARD 11)

# --------------------------------------------------------------------------
# Build options
# --------------------------------------------------------------------------
option(WITH_SNAP7    "Build with snap7 OPC-DA client support"   OFF)
option(WITH_OPEN62541 "Build with open62541 OPC-UA server support" OFF)

# --------------------------------------------------------------------------
# Source files
# --------------------------------------------------------------------------
set(SOURCES
    src/main.c
    src/config.c
    src/logger.c
    src/opcda_client.c
    src/opcua_server.c
    src/web_config.c
    third_party/cJSON/cJSON.c
)

add_executable(opcda2ua ${SOURCES})

target_include_directories(opcda2ua PRIVATE
    src
    third_party/cJSON
)

# --------------------------------------------------------------------------
# Optional: snap7
# --------------------------------------------------------------------------
if(WITH_SNAP7)
    find_library(SNAP7_LIB snap7 HINTS /usr/lib /usr/local/lib)
    if(SNAP7_LIB)
        target_compile_definitions(opcda2ua PRIVATE HAVE_SNAP7)
        target_link_libraries(opcda2ua ${SNAP7_LIB})
        message(STATUS "snap7 found: ${SNAP7_LIB}")
    else()
        message(WARNING "snap7 library not found; OPC-DA client will be a stub")
    endif()
endif()

# --------------------------------------------------------------------------
# Optional: open62541
# --------------------------------------------------------------------------
if(WITH_OPEN62541)
    set(OPEN62541_LOCAL_PREFIXES
        ${CMAKE_SOURCE_DIR}/third_party/open62541_install
    )
    if(WIN32)
        if(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
            list(INSERT OPEN62541_LOCAL_PREFIXES 0
                ${CMAKE_SOURCE_DIR}/third_party/open62541_install_win32
            )
        elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
            list(INSERT OPEN62541_LOCAL_PREFIXES 0
                ${CMAKE_SOURCE_DIR}/third_party/open62541_install_x64
            )
        elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
            list(INSERT OPEN62541_LOCAL_PREFIXES 0
                ${CMAKE_SOURCE_DIR}/third_party/open62541_install_arm64
            )
        endif()
    endif()

    find_package(open62541 CONFIG QUIET
        PATHS
            ${OPEN62541_LOCAL_PREFIXES}
            ${CMAKE_SOURCE_DIR}/third_party/open62541_install/lib/cmake/open62541
    )
    if(TARGET open62541::open62541)
        target_compile_definitions(opcda2ua PRIVATE HAVE_OPEN62541)
        target_link_libraries(opcda2ua open62541::open62541)
        message(STATUS "open62541 found via CMake package")
    else()
        if(WIN32)
            set(OPEN62541_HINTS
                ${OPEN62541_LOCAL_PREFIXES}
                $ENV{OPEN62541_ROOT}
                $ENV{OPEN62541_DIR}
            )
            find_path(OPEN62541_INCLUDE_DIR
                NAMES open62541/server.h
                HINTS ${OPEN62541_HINTS}
                PATH_SUFFIXES include
            )
            find_library(OPEN62541_LIB
                NAMES open62541 open62541d
                HINTS ${OPEN62541_HINTS}
                PATH_SUFFIXES lib bin
            )
        else()
            find_path(OPEN62541_INCLUDE_DIR
                NAMES open62541/server.h
                HINTS /usr/include /usr/local/include
            )
            find_library(OPEN62541_LIB
                NAMES open62541
                HINTS /usr/lib /usr/local/lib
            )
        endif()

        if(OPEN62541_LIB AND OPEN62541_INCLUDE_DIR)
            target_compile_definitions(opcda2ua PRIVATE HAVE_OPEN62541)
            target_include_directories(opcda2ua PRIVATE ${OPEN62541_INCLUDE_DIR})
            target_link_libraries(opcda2ua ${OPEN62541_LIB})
            message(STATUS "open62541 found: ${OPEN62541_LIB}")
        else()
            message(WARNING "open62541 not found; OPC-UA server will be a stub")
            if(WIN32)
                message(STATUS "Set OPEN62541_ROOT to your open62541 install prefix (contains include/ and lib/)")
            endif()
        endif()
    endif()
endif()

# Always need pthreads
find_package(Threads REQUIRED)
target_link_libraries(opcda2ua Threads::Threads)

if(WIN32)
    target_link_libraries(opcda2ua ole32 oleaut32 ws2_32)
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------
enable_testing()

add_executable(test_config
    tests/test_config.c
    src/config.c
    third_party/cJSON/cJSON.c
)
target_include_directories(test_config PRIVATE src third_party/cJSON)
add_test(NAME config_test COMMAND test_config)

add_executable(test_logger
    tests/test_logger.c
    src/logger.c
)
target_include_directories(test_logger PRIVATE src)
add_test(NAME logger_test COMMAND test_logger)

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------
install(TARGETS opcda2ua DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/opcda2ua)
