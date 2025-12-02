cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(http-server
    DESCRIPTION "A simple HTTP/1.1 server implementation in C++ using standard libraries"
    LANGUAGES CXX)

# Optimization flags for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto")

# Enable warnings for Debug builds
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Wpedantic")

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include_directories(src)

set(SRC_DIR src)
set(TEST_DIR test)

add_executable(HttpServer
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/http_server.cpp
    ${SRC_DIR}/http_message.cpp
)

add_executable(test_HttpServer
    ${TEST_DIR}/main.cpp
    ${SRC_DIR}/http_server.cpp
    ${SRC_DIR}/http_message.cpp
)

target_link_libraries(HttpServer PRIVATE Threads::Threads)
target_link_libraries(test_HttpServer PRIVATE Threads::Threads)
