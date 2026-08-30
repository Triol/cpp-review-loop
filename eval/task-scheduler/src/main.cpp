// main.cpp —— 演示程序：解析命令行 -> 加载配置 -> 初始化各模块
//             -> 提交一批示例任务（含周期性任务）-> 优雅停机
//             -> 打印统计结果与按 tag 的当天日志报表（可选 JSON / CSV 形式，
//                可用 --out 把本次选定的报表同时写入指定文件）。
//             --trace 开启调度器关键事件追踪（带毫秒时间戳，输出到 stderr）。

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "metrics.h"
#include "persistence.h"
#include "stats_report.h"
#include "task_scheduler.h"

namespace {

/* --json / --csv 开关：选定结构化输出后，运行过程信息转走 stderr，
   保证 stdout 只承载报表输出 */
bool g_jsonOutput = false;
bool g_csvOutput = false;

/* 运行过程信息输出：普通模式打 stdout，--json 模式打 stderr */
void logInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_jsonOutput ? stderr : stdout, fmt, args);
    va_end(args);
}

/* 模拟一段真实工作：耗时 ms 毫秒；fail 为 true 时以异常收场 */
void simulateWork(int ms, bool fail) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if (fail) {
        throw std::runtime_error("simulated task failure");
    }
}

/* 打印生效配置，方便确认加载结果 */
void printConfig(const tasksched::SchedulerConfig& cfg) {
    logInfo("[config] worker_threads       = %d\n", cfg.worker_threads);
    logInfo("[config] queue_capacity        = %zu\n", cfg.queue_capacity);
    logInfo("[config] log_directory         = %s\n", cfg.log_directory.c_str());
    logInfo("[config] log_file_prefix       = %s\n", cfg.log_file_prefix.c_str());
    logInfo("[config] persistence_enabled   = %s\n", cfg.persistence_enabled ? "true" : "false");
    logInfo("[config] trace_enabled         = %s\n", cfg.trace_enabled ? "true" : "false");
}

} // namespace

int main(int argc, char* argv[]) {
    /* ---- 0. 解析命令行：--json / --csv / --trace / --out 开关 + 可选的配置文件路径 ---- */
    const char* configPath = "scheduler.conf";
    bool configPathSet = false;
    bool traceFlag = false; /* --trace：把配置里的 trace_enabled 覆盖为 true */
    std::string outPath; /* --out 指定的报表输出文件；空串表示不写文件 */
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            g_jsonOutput = true;
        } else if (arg == "--csv") {
            g_csvOutput = true;
        } else if (arg == "--trace") {
            traceFlag = true;
        } else if (arg == "--out") {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                             "错误: %s 缺少文件路径参数\n"
                             "用法: %s [--json|--csv] [--trace] [--out <path>] [配置文件路径]\n",
                             arg.c_str(), argv[0]);
                return 1;
            }
            outPath = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr,
                         "错误: 未知参数 %s\n"
                         "用法: %s [--json|--csv] [--trace] [--out <path>] [配置文件路径]\n",
                         arg.c_str(), argv[0]);
            return 1;
        } else if (!configPathSet) {
            configPath = argv[i]; /* 第一个位置参数 = 配置文件路径（与旧版兼容） */
            configPathSet = true;
        }
    }
    if (g_jsonOutput && g_csvOutput) {
        std::fprintf(stderr,
                     "错误: --json 与 --csv 只能二选一\n"
                     "用法: %s [--json|--csv] [--trace] [--out <path>] [配置文件路径]\n",
                     argv[0]);
        return 1;
    }

    /* ---- 1. 加载配置 ---- */
    tasksched::SchedulerConfig config;
    std::string configError;
    if (!config.loadFromFile(configPath, &configError)) {
        if (configPathSet) {
            /* 显式指定的配置文件打不开属于致命错误 */
            std::fprintf(stderr, "错误: %s\n", configError.c_str());
            return 1;
        }
        logInfo("[config] 未找到 %s，使用内置默认配置\n", configPath);
        logInfo("[config] 可用键: worker_threads / queue_capacity / "
                "log_directory / log_file_prefix / persistence_enabled / trace_enabled\n");
    } else if (!configError.empty()) {
        /* 部分行解析失败：已跳过并保留默认值，给出告警但继续运行 */
        logInfo("[config] 配置解析告警:\n%s", configError.c_str());
    }
    /* 命令行 --trace 优先于配置文件：覆盖 trace_enabled 为开启 */
    if (traceFlag) {
        config.trace_enabled = true;
    }

    std::string validateError;
    if (!config.validate(&validateError)) {
        std::fprintf(stderr, "错误: 配置不合法:\n%s", validateError.c_str());
        return 1;
    }
    printConfig(config);

    /* ---- 2. 初始化指标与持久化模块 ---- */
    tasksched::Metrics metrics;

    result_writer_t* persister = nullptr;
    if (config.persistence_enabled) {
        persister = result_writer_open(config.log_directory.c_str(),
                                       config.log_file_prefix.c_str());
        if (persister == nullptr) {
            /* 目录创建/打开失败不终止程序：降级为"只统计不落盘" */
            logInfo("[persist] 初始化失败（目录不可写?），本次运行不持久化\n");
        }
    }

    /* ---- 3. 构造调度器并提交示例任务 ---- */
    std::size_t deadLetterTotal = 0; /* 死信任务数（shutdown 后取最终值） */
    {
        tasksched::TaskScheduler scheduler(config, metrics, persister);
        logInfo("[main] 启动 %d 个工作线程\n", scheduler.workerCount());

        std::vector<tasksched::TaskId> ids;
        ids.reserve(18);

        /* 10 个普通任务：按编号错开耗时，第 4、8 个故意失败，全部带 batch 标签 */
        for (int i = 1; i <= 10; ++i) {
            const int ms = (i * 13) % 40 + 5;
            const bool fail = (i == 4 || i == 8);
            ids.push_back(scheduler.submit([i, ms, fail] { simulateWork(ms, fail); },
                                           tasksched::Priority::Normal, 0, "batch"));
        }

        /* 2 个高优先级任务：应排在其余排队任务之前执行 */
        ids.push_back(scheduler.submit([] { simulateWork(10, false); },
                                       tasksched::Priority::Critical, 0, "urgent"));
        ids.push_back(scheduler.submit([] { simulateWork(10, false); },
                                       tasksched::Priority::High));

        /* 重试演示 1：前两次尝试抛异常，第三次成功（max_retries = 2） */
        const auto flakyAttempts = std::make_shared<int>(0);
        ids.push_back(scheduler.submit([flakyAttempts] {
            if (++(*flakyAttempts) < 3) {
                throw std::runtime_error("flaky failure (will retry)");
            }
        }, tasksched::Priority::High, 2, "flaky"));

        /* 重试演示 2：永远失败，重试额度用尽后才计为最终失败（max_retries = 1） */
        ids.push_back(scheduler.submit([] { throw std::runtime_error("always fails"); },
                                       tasksched::Priority::Normal, 1, "doomed"));

        /* 周期性任务演示：成功执行完自动重新入队（不消耗重试额度），
           shutdown 后不再重新入队，随停机自然终止 */
        ids.push_back(scheduler.submit([] { simulateWork(20, false); },
                                       tasksched::Priority::Normal, 0, "heartbeat",
                                       tasksched::TaskType::Periodic));

        /* 3 个低优先级任务，随后取消其中 2 个 */
        const tasksched::TaskId c1 = scheduler.submit([] { simulateWork(5, false); },
                                                      tasksched::Priority::Low);
        const tasksched::TaskId c2 = scheduler.submit([] { simulateWork(5, false); },
                                                      tasksched::Priority::Low);
        const tasksched::TaskId c3 = scheduler.submit([] { simulateWork(5, false); },
                                                      tasksched::Priority::Low);
        ids.push_back(c1);
        ids.push_back(c2);
        ids.push_back(c3);

        logInfo("[main] 共提交 %zu 个任务，尝试取消 2 个...\n", ids.size());
        logInfo("[main] cancel(%llu) -> %s\n", static_cast<unsigned long long>(c2),
                scheduler.cancel(c2) ? "ok" : "failed");
        logInfo("[main] cancel(%llu) -> %s\n", static_cast<unsigned long long>(c3),
                scheduler.cancel(c3) ? "ok" : "failed");
        logInfo("[main] cancel(%llu) -> %s（重复取消应失败）\n",
                static_cast<unsigned long long>(c2),
                scheduler.cancel(c2) ? "ok" : "failed");

        /* ---- 4. 优雅停机：拒绝新任务，等剩余任务全部做完再退出 ---- */
        scheduler.shutdown();
        logInfo("[main] shutdown 完成，队列剩余任务数 = %zu\n",
                scheduler.pendingCount());
        /* 死信统计需在调度器存活期间取好（析构后不可再查询）；
           shutdown 排空后，死信数量即为最终值。 */
        deadLetterTotal = scheduler.deadLetterCount();
    }

    /* ---- 5. 输出统计摘要 ---- */
    const tasksched::MetricsSnapshot snap = metrics.snapshot();
    logInfo("[metrics] 已执行 = %llu, 最终失败 = %llu, 已取消 = %llu, "
            "重试中的失败 = %llu, 平均耗时 = %.3f ms\n",
            static_cast<unsigned long long>(snap.executed),
            static_cast<unsigned long long>(snap.failed),
            static_cast<unsigned long long>(snap.cancelled),
            static_cast<unsigned long long>(snap.retried),
            snap.average_ms);
    logInfo("[metrics] 成功执行按优先级: Critical = %llu, High = %llu, "
            "Normal = %llu, Low = %llu\n",
            static_cast<unsigned long long>(
                snap.succeeded_by_priority[static_cast<int>(tasksched::Priority::Critical)]),
            static_cast<unsigned long long>(
                snap.succeeded_by_priority[static_cast<int>(tasksched::Priority::High)]),
            static_cast<unsigned long long>(
                snap.succeeded_by_priority[static_cast<int>(tasksched::Priority::Normal)]),
            static_cast<unsigned long long>(
                snap.succeeded_by_priority[static_cast<int>(tasksched::Priority::Low)]));
    logInfo("[metrics] 死信任务数 = %zu\n", deadLetterTotal);

    if (persister != nullptr) {
        result_writer_flush(persister);
        result_writer_close(persister);
        persister = nullptr;
    }

    /* ---- 6. 统计报表：读取当天任务日志，按 tag 汇总各状态条数与 p95 耗时 ---- */
    const tasksched::DailyTagStatsReport report =
        tasksched::buildDailyTagStats(config.log_directory, config.log_file_prefix);

    /* 按命令行开关把报表渲染成文本 / JSON / CSV 之一，统一打印到 stdout；
       --out 指定了文件时，把同一份内容再写入该文件 */
    std::string rendered;
    if (g_csvOutput) {
        rendered = tasksched::dailyTagStatsToCsv(report);
    } else if (g_jsonOutput) {
        rendered = tasksched::dailyTagStatsToJson(report);
    } else {
        rendered = tasksched::dailyTagStatsToText(report);
    }
    std::fwrite(rendered.data(), 1, rendered.size(), stdout);
    std::fputc('\n', stdout);

    if (!outPath.empty()) {
        bool writeOk = true;
        std::FILE* outFile = std::fopen(outPath.c_str(), "w");
        if (outFile == nullptr) {
            writeOk = false;
        } else {
            writeOk = std::fwrite(rendered.data(), 1, rendered.size(), outFile) ==
                          rendered.size() &&
                      std::fputc('\n', outFile) != EOF;
            if (std::fclose(outFile) != 0) {
                writeOk = false;
            }
        }
        if (!writeOk) {
            std::fprintf(stderr, "错误: 报表写入文件失败: %s\n", outPath.c_str());
            return 1;
        }
        logInfo("[stats] 报表已同时写入文件: %s\n", outPath.c_str());
    }
    return 0;
}
