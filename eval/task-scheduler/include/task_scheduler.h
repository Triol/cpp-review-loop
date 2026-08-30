#pragma once
// task_scheduler.h —— 核心调度器：有界优先级任务队列 + N 个工作线程。
//
// 能力：
//   * submit()   提交可调用对象（lambda、函数对象、std::function……），
//                可带优先级、最大重试次数与标签 tag；
//   * 任务抛出异常后自动重新入队重试，重试额度用尽才计为最终失败；
//   * 任务分一次性（one-shot）与周期性（periodic）：周期性任务成功执行完
//     自动重新入队（不消耗重试额度），停机后不再重新入队；
//   * 内部 N 个工作线程（N 来自 SchedulerConfig）从队列抢占任务执行；
//   * cancel()   按任务 ID 取消"尚未开始"的任务（懒取消：出队时过滤）；
//   * shutdown() 拒绝新任务，阻塞到队列排空、全部工作线程退出；
//   * 最终失败（重试额度用尽）的任务进入死信队列，可经 deadLetterCount() /
//     deadLetterSummary() 查询；
//   * config.trace_enabled 开启时，把关键事件（入队/出队/开始/完成/重试
//     重入队/周期重入队/取消/拒绝）以统一格式输出到 stderr（见 trace.h）。
//
// 模块协作：每次任务尝试结束后，通过 Metrics 记录结果、通过 legacy C 接口
// result_writer_append_ex3 把结果（含 tag、任务类型 type、执行线程编号
// worker 与整数微秒耗时 duration_us）落盘（persister 传 nullptr 可关闭持久化）。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "metrics.h"
#include "persistence.h"
#include "priority.h"
#include "task_type.h"

namespace tasksched {

/* 任务 ID：从 1 开始单调递增；0 保留，表示"无效 / 提交被拒绝"。 */
using TaskId = std::uint64_t;

/* 任务优先级 Priority 已下沉到 priority.h（调度器与 Metrics 共用的最小头）。 */

/* 死信条目：一个最终失败（重试额度用尽仍抛异常）的任务。
   detail 与该任务 FAILED 持久化日志行的 detail 字段一致（配置了重试时
   含"attempt x of y"前缀与异常信息），排障时可与日志行直接对照。 */
struct DeadLetterEntry {
    TaskId id = 0;
    std::string detail;
};

class TaskScheduler {
public:
    /*
     * 构造并启动工作线程。
     * metrics   : 调用方持有的指标统计器（引用，生命周期须覆盖调度器）；
     * persister : legacy 持久化句柄，可为 nullptr（不落盘）。
     */
    TaskScheduler(const SchedulerConfig& config, Metrics& metrics,
                  result_writer_t* persister);

    /* 析构等价于 shutdown()：拒绝新任务并等待剩余任务执行完毕。 */
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    /*
     * 提交一个任务。返回全局唯一任务 ID；队列已满或已停机时返回 0（拒绝）。
     * max_retries : 执行抛异常后的最大自动重试次数（默认 0 = 不重试，负值按 0 处理）；
     *               每次尝试都照常计入指标并落盘，重试中的失败以 status=RETRYING
     *               落盘，额度用尽后才计为最终失败（FAILED）。
     * tag         : 任务标签（默认空串），随执行结果一并写入持久化日志行。
     * type        : 任务类型（默认一次性 OneShot）。周期性（Periodic）任务
     *               成功执行完自动重新入队（attempt 归位、不消耗重试额度），
     *               停机后不再重新入队；失败仍走常规重试 / 最终失败路径。
     */
    TaskId submit(std::function<void()> task, Priority priority = Priority::Normal,
                  int max_retries = 0, std::string tag = std::string(),
                  TaskType type = TaskType::OneShot);

    /* 模板入口：接受任意可调用对象，统一转成 std::function 后提交。 */
    template <class F>
    TaskId submit(F&& task, Priority priority = Priority::Normal, int max_retries = 0,
                  std::string tag = std::string(),
                  TaskType type = TaskType::OneShot) {
        return submit(std::function<void()>(std::forward<F>(task)), priority,
                      max_retries, std::move(tag), type);
    }

    /*
     * 取消一个尚未开始执行的任务。
     * 返回 true  : 任务仍在排队，已标记取消（出队时将被丢弃并计入统计）；
     * 返回 false : 任务不存在、已开始执行或已完成，无法取消。
     */
    bool cancel(TaskId id);

    /* 停止接收新任务；阻塞直到队列排空、所有工作线程退出。幂等，可重复调用。 */
    void shutdown();

    /* 当前仍在队列中排队、尚未被工作线程取走的任务数。 */
    std::size_t pendingCount() const;

    /* 已进入死信队列的任务数（= 最终失败任务数，与 Metrics 的 failed
       计数口径一致；shutdown 返回后即为最终值）。 */
    std::size_t deadLetterCount() const;

    /* 死信快照：每条最终失败任务的 ID 与失败详情（顺序 = 进入死信的
       先后顺序）。shutdown 返回后调用即得完整死信列表。 */
    std::vector<DeadLetterEntry> deadLetterSummary() const;

    /* 工作线程数量（来自配置）。 */
    int workerCount() const noexcept { return static_cast<int>(workers_.size()); }

private:
    /* 队列元素：优先级 + 同优先级内的 FIFO 序号 + 重试信息 + 类型 + 标签 + 任务体。 */
    struct Task {
        TaskId id = 0;
        int priority = 0;
        std::uint64_t seq = 0;
        int max_retries = 0; // 提交时设定的最大重试次数（总尝试上限 = max_retries + 1）
        int attempt = 1;     // 当前是第几次尝试（从 1 起计）
        TaskType type = TaskType::OneShot; // 一次性 / 周期性（随执行结果落盘）
        std::string tag;     // 用户标签，随执行结果落盘
        std::function<void()> fn;
    };

    /* 堆比较器：值"大"者先出队 —— 优先级高者大；同优先级 seq 小者大。 */
    struct TaskGreater {
        bool operator()(const Task& a, const Task& b) const noexcept;
    };

    /* worker_index : 本工作线程的编号（0..N-1），随执行结果写入日志行 worker 字段 */
    void workerLoop(int worker_index);        // 工作线程主循环：取任务 -> 执行 -> 上报
    void runOne(Task task, int worker_index); // 执行单个尝试并上报 metrics / persistence
    void requeueForRetry(Task t); // 失败任务重试：重新入队（不受队列容量限制）
    void requeuePeriodic(Task t); // 周期任务成功后重新入队（停机则丢弃，不占容量）

    SchedulerConfig config_;     // 配置副本（避免依赖调用方配置对象存活）
    Metrics& metrics_;           // 指标统计（必填）
    result_writer_t* persister_; // legacy 持久化句柄（可空，表示关闭落盘）

    mutable std::mutex mutex_;   // 保护以下共享状态
    std::condition_variable cv_; // 有新任务 / 停机时唤醒工作线程
    std::vector<Task> heap_;     // 优先级堆（std::push_heap / pop_heap 维护）
    std::unordered_set<TaskId> queuedIds_;    // 堆中现存任务 ID（支持按 ID 取消）
    std::unordered_set<TaskId> cancelledIds_; // 已取消、等出队时过滤
    std::vector<DeadLetterEntry> deadLetters_; // 死信队列：最终失败任务（先进先出序）
    bool stopping_ = false;      // 停机标志：置位后拒绝新任务

    std::vector<std::thread> workers_;
    std::atomic<TaskId> nextId_{1}; // 任务 ID 发放器（ID 单调递增）
};

} // namespace tasksched
