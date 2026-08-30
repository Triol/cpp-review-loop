#pragma once
// trace.h —— 追踪日志模块（header-only）：--trace 开启后，调度器把关键事件
// 以统一格式输出到 stderr，供运维排障使用。
//
// 单行格式（时间戳为本地墙钟时间，毫秒精度）：
//     <YYYY-MM-DD HH:MM:SS.mmm> [trace] <事件名> task=<id><字段...>
// 事件名与字段的约定（事件名小写下划线风格，便于 grep）：
//     enqueue           任务入队            priority=<low|normal|high|critical>
//                                           type=<oneshot|periodic> retries=<n>
//     dequeue           出队                worker=<n> priority=<...>
//     start             开始执行            worker=<n> attempt=<n>
//     finish            执行完成            worker=<n> ok=<0|1> duration_ms=<x.xxx>
//     retry_requeue     重试重新入队        attempt=<n>（下一次尝试序号）
//     periodic_requeue  周期任务重新入队
//     cancel            被取消
//     reject            被拒绝              reason=<queue_full|stopped>
//                                           （被拒任务没有 ID，按约定记 task=0）
//
// 实现说明：开关判断（config.trace_enabled）由调用方在各埋点处完成——
// 关闭时连字段字符串都不必构造，追踪为默认零开销。整行拼好后单次 fputs
// 写 stderr，多线程并发时行与行不会交错。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace tasksched {

/* time_t -> 本地时间（跨平台封装：Windows 用 localtime_s，POSIX 用 localtime_r，
   与 persistence.cpp 同款，保证多线程下线程安全） */
inline void traceLocaltime(std::time_t now, std::tm* out) {
#if defined(_WIN32)
    localtime_s(out, &now);
#else
    localtime_r(&now, out);
#endif
}

/* 当前墙钟时间戳 "YYYY-MM-DD HH:MM:SS.mmm"（毫秒精度，本地时间） */
inline std::string traceTimestamp() {
    using std::chrono::system_clock;
    const auto now = system_clock::now();
    const std::time_t sec = system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);
    std::tm tmv;
    traceLocaltime(sec, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return std::string(buf);
}

/*
 * 输出一行追踪日志到 stderr。
 * event   : 事件名（见文件头注释的事件表）；
 * task_id : 任务 ID；被拒绝的提交没有 ID，按 TaskId 的约定传 0；
 * fields  : 附加字段串（调用方拼好，每个字段前带一个空格），可为空串。
 */
inline void traceLog(const char* event, std::uint64_t task_id,
                     const std::string& fields) {
    std::string line = traceTimestamp();
    line += " [trace] ";
    line += event;
    line += " task=";
    line += std::to_string(task_id);
    line += fields;
    line += '\n';
    std::fputs(line.c_str(), stderr); /* 单次调用输出整行，避免多线程行交错 */
}

} // namespace tasksched
