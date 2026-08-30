# 核心静态库：config + metrics(header-only) + persistence(legacy C) + scheduler + stats_report
add_library(scheduler_core STATIC
    config.cpp
    persistence.cpp
    stats_report.cpp
    task_scheduler.cpp
)
target_include_directories(scheduler_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(scheduler_core PUBLIC Threads::Threads)

# 演示可执行程序：加载配置 -> 提交示例任务 -> 优雅停机 -> 打印统计
add_executable(task_scheduler_demo main.cpp)
target_link_libraries(task_scheduler_demo PRIVATE scheduler_core)
