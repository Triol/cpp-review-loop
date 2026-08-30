#pragma once
// config.h —— 配置模块：从 key=value 文本文件加载调度器配置。
//
// 支持的配置键（均可缺省，缺省时使用默认值）：
//   worker_threads      工作线程数（1..256，默认 4）
//   queue_capacity      待执行任务队列容量（>=1，默认 1024）
//   log_directory       持久化日志目录（默认 "logs"）
//   log_file_prefix     日志文件名前缀（默认 "task_results"）
//   persistence_enabled 是否把任务结果写入日志文件（1/0，默认 1）
//   trace_enabled       是否向 stderr 输出关键事件追踪日志（1/0，默认 0，
//                       命令行 --trace 可覆盖为开启；格式见 trace.h）
//
// 文件格式约定：
//   * 每行一条 "key=value"，'=' 两侧的空白忽略；
//   * '#' 开头的行视为注释，空行跳过；
//   * 未知键只忽略、不报错（兼容旧配置文件）。

#include <cstddef>
#include <string>

namespace tasksched {

struct SchedulerConfig {
    int worker_threads = 4;
    std::size_t queue_capacity = 1024;
    std::string log_directory = "logs";
    std::string log_file_prefix = "task_results";
    bool persistence_enabled = true;
    bool trace_enabled = false;

    /*
     * 从 path 读取配置。文件打不开时返回 false 并填写 *error；
     * 单行解析失败不致命：跳过该行并把告警追加到 *error，其余键照常生效。
     */
    bool loadFromFile(const std::string& path, std::string* error = nullptr);

    /* 校验各配置项取值范围；失败返回 false 并填写 *error。 */
    bool validate(std::string* error = nullptr) const;
};

} // namespace tasksched
