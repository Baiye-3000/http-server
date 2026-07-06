cmake_minimum_required(VERSION 3.10)

# CentOS 8 默认 GCC 8 不支持 C++20，优先使用 gcc-toolset-10
if(NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "/opt/rh/gcc-toolset-10/root/usr/bin/g++")
    set(CMAKE_CXX_COMPILER "/opt/rh/gcc-toolset-10/root/usr/bin/g++")
endif()

project(simple_http_server C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# Gumbo: HTML5 标准解析 → DOM 树
# https://codeberg.org/gumbo-parser/gumbo-parser
FetchContent_Declare(
    gumbo
    GIT_REPOSITORY https://codeberg.org/gumbo-parser/gumbo-parser.git
    GIT_TAG 0.13.2
)
FetchContent_MakeAvailable(gumbo)

add_library(gumbo STATIC
    ${gumbo_SOURCE_DIR}/src/attribute.c
    ${gumbo_SOURCE_DIR}/src/char_ref.c
    ${gumbo_SOURCE_DIR}/src/error.c
    ${gumbo_SOURCE_DIR}/src/parser.c
    ${gumbo_SOURCE_DIR}/src/string_buffer.c
    ${gumbo_SOURCE_DIR}/src/string_piece.c
    ${gumbo_SOURCE_DIR}/src/tag.c
    ${gumbo_SOURCE_DIR}/src/tokenizer.c
    ${gumbo_SOURCE_DIR}/src/utf8.c
    ${gumbo_SOURCE_DIR}/src/util.c
    ${gumbo_SOURCE_DIR}/src/vector.c
)
target_include_directories(gumbo PUBLIC ${gumbo_SOURCE_DIR}/src)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Boost 1.66 REQUIRED COMPONENTS system)

# =====================
# 主程序
# =====================
add_executable(simple_http_server
    src/main.cpp
    src/http_server.cpp
    src/html_parser.cpp
)

target_include_directories(simple_http_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(simple_http_server PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(simple_http_server PRIVATE gumbo nlohmann_json::nlohmann_json ${Boost_LIBRARIES} pthread)

# =====================
# 测试
# =====================
add_executable(test_html_parser
    tests/test_html_parser.cpp
    src/html_parser.cpp
)

target_include_directories(test_html_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_html_parser PRIVATE gumbo nlohmann_json::nlohmann_json)

add_executable(test_html_www_pages
    tests/test_html_www_pages.cpp
    src/html_parser.cpp
)

target_include_directories(test_html_www_pages PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_html_www_pages PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(test_html_www_pages PRIVATE gumbo nlohmann_json::nlohmann_json)

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_http_server.cpp")
    add_executable(test_http_server tests/test_http_server.cpp)
    target_include_directories(test_http_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_http_server PRIVATE pthread)
endif()

add_executable(test_http_server_stress
    tests/test_http_server_stress.cpp
)

target_link_libraries(test_http_server_stress PRIVATE pthread)

add_executable(test_http_server_sticky
    tests/test_http_server_sticky.cpp
)

target_link_libraries(test_http_server_sticky PRIVATE pthread)

add_executable(test_http_server_crash
    tests/test_http_server_crash.cpp
)

target_link_libraries(test_http_server_crash PRIVATE pthread)

enable_testing()
add_test(NAME html_parser_test COMMAND test_html_parser)
add_test(NAME html_www_pages_test COMMAND test_html_www_pages)
add_test(NAME http_server_sticky COMMAND test_http_server_sticky)
add_test(NAME http_server_crash COMMAND test_http_server_crash)
if(TARGET test_http_server)
    add_test(NAME http_server_e2e COMMAND test_http_server)
endif()
add_test(NAME http_server_stress COMMAND test_http_server_stress)
