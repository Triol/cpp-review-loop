#pragma once
// metrics.h —— 指标模块：线程安全地统计已执行 / 已取消 / 失败 / 重试中的
// 尝试次数、按优先级的成功执行计数，以及平均耗时。
//
// 计数口径：
//   * executed  成功执行完毕（未抛异常）的任务数；
//   * failed    最终失败（重试额度用尽仍抛异常）的任务数；
//   * retried   失败后重新入队等待重试的尝试次数（这类尝试不计入 failed）；
//   * succeeded_by_priority  按优先级统计的成功执行计数。
//
// 实现说明：所有计数都是 std::atomic，工作线程无锁并发更新即可
// （relaxed 序足够——这里只关心最终汇总值，不依赖各计数间的先后关系），
// 读取端直接原子读取。

#include <atomic>
#include <cstdint>

#include "priority.h"

namespace tasksched {

/* 优先级档位数（与 Priority 枚举的取值个数保持一致） */
inline constexpr int kPriorityLevelCount = 4;
static_assert(static_cast<int>(Priority::Critical) == kPriorityLevelCount - 1,
              "Priority 枚举取值变化时需同步 kPriorityLevelCount");

/* 某一时刻的指标快照。各字段独立读取，不保证严格同拍，仅用于展示/监控。 */
struct MetricsSnapshot {
    std::uint64_t executed = 0;   // 成功执行完毕（未抛异常）的任务数
    std::uint64_t failed = 0;     // 最终失败（重试额度用尽仍抛异常）的任务数
    std::uint64_t cancelled = 0;  // 提交后、执行前被成功取消的任务数
    std::uint64_t retried = 0;    // 失败后重新入队等待重试的尝试次数
    double average_ms = 0.0;      // 已执行尝试（含失败与重试尝试）的平均耗时，毫秒
    std::uint64_t succeeded_by_priority[kPriorityLevelCount] = {
        0, 0, 0, 0};              // 按优先级的成功执行数（下标 = Priority 枚举值）
};

/* 线程安全的任务指标统计器。 */
class Metrics {
public:
    /*
     * 记录一次任务尝试结束（终态：成功或最终失败）。
     * duration_ns : 本次执行耗时（纳秒）
     * ok          : 是否成功（未抛异常）。失败样本同样计入平均耗时。
     * priority    : 任务优先级；成功样本同时计入对应优先级的成功计数。
     */
    void recordFinished(std::uint64_t duration_ns, bool ok, Priority priority) noexcept {
        total_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
        if (ok) {
            executed_.fetch_add(1, std::memory_order_relaxed);
            succeeded_[static_cast<int>(priority)].fetch_add(1, std::memory_order_relaxed);
        } else {
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /*
     * 记录一次失败但会重新入队重试的尝试：
     * 耗时照常计入平均耗时，但不计入最终失败（failed）。
     */
    void recordRetried(std::uint64_t duration_ns) noexcept {
        total_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
        retried_.fetch_add(1, std::memory_order_relaxed);
    }

    /* 记录一次成功取消。 */
    void recordCancelled() noexcept {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t executedCount() const noexcept {
        return executed_.load(std::memory_order_relaxed);
    }

    std::uint64_t failedCount() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

    std::uint64_t cancelledCount() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    std::uint64_t retriedCount() const noexcept {
        return retried_.load(std::memory_order_relaxed);
    }

    /* 指定优先级的成功执行计数。 */
    std::uint64_t succeededCount(Priority priority) const noexcept {
        return succeeded_[static_cast<int>(priority)].load(std::memory_order_relaxed);
    }

    /* 平均执行耗时（毫秒）；还没有样本时返回 0.0。 */
    double averageExecutionMs() const noexcept {
        const std::uint64_t samples =
            executed_.load(std::memory_order_relaxed) +
            failed_.load(std::memory_order_relaxed) +
            retried_.load(std::memory_order_relaxed);
        if (samples == 0) return 0.0;
        const double ms =
            static_cast<double>(total_ns_.load(std::memory_order_relaxed)) / 1.0e6;
        return ms / static_cast<double>(samples);
    }

    /* 一次性取回全部指标，便于打印/上报。 */
    MetricsSnapshot snapshot() const noexcept {
        MetricsSnapshot snap;
        snap.executed = executedCount();
        snap.failed = failedCount();
        snap.cancelled = cancelledCount();
        snap.retried = retriedCount();
        snap.average_ms = averageExecutionMs();
        for (int i = 0; i < kPriorityLevelCount; ++i) {
            snap.succeeded_by_priority[i] =
                succeeded_[i].load(std::memory_order_relaxed);
        }
        return snap;
    }

private:
    std::atomic<std::uint64_t> executed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::atomic<std::uint64_t> cancelled_{0};
    std::atomic<std::uint64_t> retried_{0};   // 失败后重新入队重试的尝试次数
    std::atomic<std::uint64_t> succeeded_[kPriorityLevelCount] = {}; // 按优先级成功计数
    std::atomic<std::uint64_t> total_ns_{0}; // 已执行尝试的累计耗时（纳秒）
};

} // namespace tasksched
