cmake_minimum_required(VERSION 3.10)

# CentOS 8 默认 GCC 8 不支持 C++20，优先使用 gcc-toolset-10
if(NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "/opt/rh/gcc-toolset-10/root/usr/bin/g++")
    set(CMAKE_CXX_COMPILER "/opt/rh/gcc-toolset-10/root/usr/bin/g++")
endif()

project(simple_http_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.14
)
FetchContent_MakeAvailable(pugixml)

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
target_link_libraries(simple_http_server PRIVATE pugixml::pugixml)

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
target_link_libraries(test_html_parser PRIVATE pugixml::pugixml)

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
target_link_libraries(test_html_www_pages PRIVATE pugixml::pugixml)

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_http_server.cpp")
    add_executable(test_http_server tests/test_http_server.cpp)
    target_include_directories(test_http_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_http_server PRIVATE pthread)
endif()

add_executable(test_http_server_stress
    tests/test_http_server_stress.cpp
)

target_link_libraries(test_http_server_stress PRIVATE pthread)

enable_testing()
add_test(NAME html_parser_test COMMAND test_html_parser)
add_test(NAME html_www_pages_test COMMAND test_html_www_pages)
if(TARGET test_http_server)
    add_test(NAME http_server_e2e COMMAND test_http_server)
endif()
add_test(NAME http_server_stress COMMAND test_http_server_stress)
