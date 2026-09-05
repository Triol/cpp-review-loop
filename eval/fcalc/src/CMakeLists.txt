# fcalc 库：词法分析 / 语法分析 / 求值
add_library(fcalc STATIC
    lexer.cpp
    parser.cpp
    evaluator.cpp
)
target_include_directories(fcalc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# 演示程序：对若干公式求值并打印
add_executable(fcalc_demo main.cpp)
target_link_libraries(fcalc_demo PRIVATE fcalc)
