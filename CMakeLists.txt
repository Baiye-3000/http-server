cmake_minimum_required(VERSION 3.10)
project(simple_http_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(test_http_server
    tests/test_http_server.cpp
)

target_include_directories(test_http_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_http_server PRIVATE pthread)

enable_testing()
add_test(NAME html_parser_test COMMAND test_html_parser)
add_test(NAME http_server_e2e COMMAND test_http_server)