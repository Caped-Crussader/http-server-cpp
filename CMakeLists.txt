cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(http-server
    DESCRIPTION "A simple HTTP/1.1 server implementation in C++ using standard libraries"
    LANGUAGES CXX)

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
