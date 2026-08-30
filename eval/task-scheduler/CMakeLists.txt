# 进程内任务调度器 —— 根构建脚本
cmake_minimum_required(VERSION 3.10)
project(task_scheduler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 未显式指定构建类型时默认 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 统一的告警等级；MSVC 下 /utf-8 保证源码中的中文注释按 UTF-8 解析
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 调度器的工作线程依赖系统线程库
find_package(Threads REQUIRED)

add_subdirectory(src)
