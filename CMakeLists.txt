cmake_minimum_required(VERSION 3.10)
project(OpenC-DA2UA C)

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
    find_library(OPEN62541_LIB open62541 HINTS /usr/lib /usr/local/lib)
    find_path(OPEN62541_INCLUDE_DIR open62541/server.h
              HINTS /usr/include /usr/local/include)
    if(OPEN62541_LIB AND OPEN62541_INCLUDE_DIR)
        target_compile_definitions(opcda2ua PRIVATE HAVE_OPEN62541)
        target_include_directories(opcda2ua PRIVATE ${OPEN62541_INCLUDE_DIR})
        target_link_libraries(opcda2ua ${OPEN62541_LIB})
        message(STATUS "open62541 found: ${OPEN62541_LIB}")
    else()
        message(WARNING "open62541 not found; OPC-UA server will be a stub")
    endif()
endif()

# Always need pthreads
find_package(Threads REQUIRED)
target_link_libraries(opcda2ua Threads::Threads)

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
