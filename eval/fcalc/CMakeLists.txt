cmake_minimum_required(VERSION 3.10)
project(fcalc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

# 库与演示程序
add_subdirectory(src)

# 断言式测试运行器（无第三方库）
add_executable(fcalc_tests tests/test_main.cpp)
target_link_libraries(fcalc_tests PRIVATE fcalc)
add_test(NAME fcalc_tests COMMAND fcalc_tests)
