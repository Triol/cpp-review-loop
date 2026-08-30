// task_scheduler.cpp —— TaskScheduler 实现。

#include "task_scheduler.h"

#include <chrono>
#include <utility>

#include "trace.h"

namespace tasksched {

/* ---- 内部工具 ---- */

/* “值大者优先出队”：先比优先级（Critical > Normal），同优先级按提交顺序 FIFO。 */
bool TaskScheduler::TaskGreater::operator()(const Task& a, const Task& b) const noexcept {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.seq > b.seq;
}

/* ---- 构造 / 析构 ---- */

TaskScheduler::TaskScheduler(const SchedulerConfig& config, Metrics& metrics,
                             result_writer_t* persister)
    : config_(config), metrics_(metrics), persister_(persister) {
    workers_.reserve(static_cast<std::size_t>(config_.worker_threads));
    try {
        for (int i = 0; i < config_.worker_threads; ++i) {
            workers_.emplace_back(&TaskScheduler::workerLoop, this, i);
        }
    } catch (...) {
        /* 线程创建中途失败：置停机标志、回收已启动的线程，再向上传播。
           不做清理会让这些线程在析构后继续访问 this，成为悬空引用。 */
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (std::thread& w : workers_) {
            if (w.joinable()) w.join();
        }
        throw;
    }
}

TaskScheduler::~TaskScheduler() {
    /* 析构即停机：不再接收新任务，等剩余任务做完（与 shutdown 语义一致） */
    shutdown();
}

/* ---- 对外接口 ---- */

TaskId TaskScheduler::submit(std::function<void()> task, Priority priority,
                             int max_retries, std::string tag, TaskType type) {
    if (!task) return 0; // 空可调用对象直接拒绝

    TaskId id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* 已停机或队列已满：拒绝提交（返回 0 表示失败）。
           拒绝是罕见路径，追踪输出直接放在临界区内（被拒任务没有 ID，
           按约定记 task=0）。 */
        if (stopping_ || heap_.size() >= config_.queue_capacity) {
            if (config_.trace_enabled) {
                traceLog("reject", 0,
                         stopping_ ? " reason=stopped" : " reason=queue_full");
            }
            return 0;
        }

        id = nextId_.fetch_add(1, std::memory_order_relaxed);
        Task item;
        item.id = id;
        item.priority = static_cast<int>(priority);
        item.seq = id; /* ID 单调递增，可直接充当同优先级内的 FIFO 序号 */
        item.max_retries = (max_retries > 0) ? max_retries : 0; /* 负值按 0 处理 */
        item.attempt = 1;
        item.type = type;
        item.tag = std::move(tag);
        item.fn = std::move(task);

        heap_.push_back(std::move(item));
        std::push_heap(heap_.begin(), heap_.end(), TaskGreater{});
        queuedIds_.insert(id);
    }
    cv_.notify_one(); /* 只需唤醒一个空闲工作线程 */

    /* 入队成功：输出追踪日志（参数值在锁外取用，字段串仅在追踪开启时构造） */
    if (config_.trace_enabled) {
        traceLog("enqueue", id,
                 std::string(" priority=") + priorityName(priority) +
                     " type=" + taskTypeName(type) +
                     " retries=" + std::to_string((max_retries > 0) ? max_retries : 0));
    }
    return id;
}

bool TaskScheduler::cancel(TaskId id) {
    std::string cancelledTag;
    TaskType cancelledType = TaskType::OneShot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* 只有仍在排队中的任务可以取消：ID 不在 queuedIds_ 里，
           意味着任务不存在、已被取走执行或早已完成。 */
        if (queuedIds_.erase(id) == 0) {
            return false;
        }
        /* 懒取消：任务仍留在堆里，顺手取出它的 tag 与类型，供下面的
           CANCELLED 日志行使用（让取消记录也能按 tag / type 统计）。 */
        for (const Task& t : heap_) {
            if (t.id == id) {
                cancelledTag = t.tag;
                cancelledType = t.type;
                break;
            }
        }
        cancelledIds_.insert(id); /* 懒取消：任务留在堆里，出队时过滤丢弃 */
    }

    /* 统计与持久化放在锁外完成，缩短临界区（持久化内部有自己的锁） */
    metrics_.recordCancelled();
    if (persister_ != nullptr) {
        /* 取消发生在提交者线程、任务未经任何 worker 执行：沿用旧接口 ex2，
           日志行 worker 字段记为 "-"、duration_us 字段记为 "-"。 */
        result_writer_append_ex2(persister_, static_cast<long long>(id),
                                 "CANCELLED", -1.0, cancelledTag.c_str(),
                                 taskTypeName(cancelledType),
                                 "cancelled before start");
    }
    if (config_.trace_enabled) {
        traceLog("cancel", id, "");
    }
    return true;
}

void TaskScheduler::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return; /* 幂等：重复调用 / 析构后再调用直接返回 */
        stopping_ = true;
    }
    /* 唤醒全部工作线程：它们会把队列中剩余任务做完后自行退出，
       join 返回即代表"全部任务执行完毕"。 */
    cv_.notify_all();
    for (std::thread& w : workers_) {
        if (w.joinable()) w.join();
    }
}

std::size_t TaskScheduler::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

/* ---- 内部逻辑 ---- */

void TaskScheduler::workerLoop(int worker_index) {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            /* 停机或队列非空时醒来；shutdown 后必须等队列排空才能退出 */
            cv_.wait(lock, [this] { return stopping_ || !heap_.empty(); });
            if (heap_.empty()) {
                return; /* stopping_ 且无剩余任务：本工作线程退出 */
            }

            std::pop_heap(heap_.begin(), heap_.end(), TaskGreater{});
            task = std::move(heap_.back());
            heap_.pop_back();
            queuedIds_.erase(task.id); /* 已离开队列，不再允许按 ID 取消 */

            /* 懒取消：出队时发现已被 cancel() 标记，直接丢弃
               （取消计数已在 cancel() 中记录，这里不再重复统计） */
            if (cancelledIds_.erase(task.id) > 0) {
                continue;
            }
        }
        if (config_.trace_enabled) {
            traceLog("dequeue", task.id,
                     " worker=" + std::to_string(worker_index) +
                         " priority=" + priorityName(static_cast<Priority>(task.priority)));
        }
        runOne(std::move(task), worker_index); /* 注意：执行任务放在锁外 */
    }
}

void TaskScheduler::runOne(Task task, int worker_index) {
    if (config_.trace_enabled) {
        traceLog("start", task.id,
                 " worker=" + std::to_string(worker_index) +
                     " attempt=" + std::to_string(task.attempt));
    }
    const auto start = std::chrono::steady_clock::now();

    bool ok = true;
    std::string detail;
    try {
        task.fn();
    } catch (const std::exception& e) {
        ok = false;
        detail = e.what();
    } catch (...) {
        ok = false;
        detail = "unknown exception";
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const std::uint64_t ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    /* 落盘耗时改为整数微秒（duration_us 字段，由旧版毫秒小数更名而来） */
    const long long us = static_cast<long long>(ns / 1000);

    /* 执行完成（无论成败，每次尝试各记一条）：含本次尝试的耗时 */
    if (config_.trace_enabled) {
        char duration[32];
        std::snprintf(duration, sizeof(duration), "%.3f",
                      static_cast<double>(ns) / 1.0e6);
        traceLog("finish", task.id,
                 " worker=" + std::to_string(worker_index) +
                     " ok=" + (ok ? "1" : "0") + " duration_ms=" + duration);
    }

    /* 配置了重试的任务在日志 detail 里带上尝试序号，便于区分各次尝试 */
    std::string persistDetail;
    if (task.max_retries > 0) {
        persistDetail = "attempt " + std::to_string(task.attempt) + " of " +
                        std::to_string(task.max_retries + 1);
        if (!detail.empty()) persistDetail += ": " + detail;
    } else {
        persistDetail = detail;
    }

    /* 失败且还有重试额度：本次尝试按"重试中的失败"上报后重新入队，
       不计入最终失败（只有重试额度用尽的那次才计为 FAILED）。 */
    if (!ok && task.attempt <= task.max_retries) {
        metrics_.recordRetried(ns);
        if (persister_ != nullptr) {
            result_writer_append_ex3(persister_, static_cast<long long>(task.id),
                                     "RETRYING", us,
                                     task.tag.c_str(), taskTypeName(task.type),
                                     worker_index, persistDetail.c_str());
        }
        requeueForRetry(std::move(task));
        return;
    }

    /* 模块间协作的核心路径：执行结果 -> 指标统计 + legacy 持久化 */
    metrics_.recordFinished(ns, ok, static_cast<Priority>(task.priority));

    /* 最终失败（重试额度用尽，或本就未配置重试）：进入死信队列。
       入库不依赖持久化开关，与 Metrics 的 failed 计数口径一致；
       detail 复用 persistDetail，与 FAILED 日志行的 detail 字段保持一致。 */
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        deadLetters_.push_back(DeadLetterEntry{task.id, persistDetail});
    }

    if (persister_ != nullptr) {
        /* 持久化是尽力而为：单条写入失败不影响调度主流程，返回值忽略 */
        result_writer_append_ex3(persister_, static_cast<long long>(task.id),
                                 ok ? "OK" : "FAILED", us,
                                 task.tag.c_str(), taskTypeName(task.type),
                                 worker_index, persistDetail.c_str());
    }

    /* 周期性任务：成功执行完自动重新入队开启下一轮（不消耗重试额度）。
       失败的周期任务走上面的重试 / 最终失败路径，不重新入队；
       停机后 requeuePeriodic 内部会丢弃任务，保证 shutdown 能排空队列。 */
    if (ok && task.type == TaskType::Periodic) {
        requeuePeriodic(std::move(task));
    }
}

void TaskScheduler::requeueForRetry(Task task) {
    const TaskId id = task.id; /* task 即将被移动，先取出 ID */
    const int nextAttempt = task.attempt + 1; /* 重新入队后的尝试序号 */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task.attempt += 1; /* 下一次尝试的序号 */
        /* 重试入队不检查队列容量与停机标志：该任务此前已被接纳，且
           shutdown 的语义是"排空所有已接纳任务"，重试也应执行完毕。 */
        heap_.push_back(std::move(task));
        std::push_heap(heap_.begin(), heap_.end(), TaskGreater{});
        queuedIds_.insert(id); /* 重试排队期间仍允许懒取消 */
    }
    cv_.notify_one();
    if (config_.trace_enabled) {
        traceLog("retry_requeue", id, " attempt=" + std::to_string(nextAttempt));
    }
}

void TaskScheduler::requeuePeriodic(Task task) {
    const TaskId id = task.id; /* task 即将被移动，先取出 ID */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /* 停机后不再重新入队：周期任务做完当前一轮即自然终止，
           否则 shutdown 的"排空已接纳任务"语义永远无法满足。 */
        if (stopping_) {
            return;
        }
        task.attempt = 1; /* 新一轮执行：尝试序号归位，不消耗重试额度 */
        /* 与重试一致：该任务此前已被接纳，重新入队不检查队列容量。 */
        heap_.push_back(std::move(task));
        std::push_heap(heap_.begin(), heap_.end(), TaskGreater{});
        queuedIds_.insert(id); /* 排队期间仍允许懒取消（可用来停掉周期任务） */
    }
    cv_.notify_one();
    if (config_.trace_enabled) {
        traceLog("periodic_requeue", id, "");
    }
}

/* ---- 死信队列查询接口 ---- */

std::size_t TaskScheduler::deadLetterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deadLetters_.size();
}

std::vector<DeadLetterEntry> TaskScheduler::deadLetterSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deadLetters_;
}

} // namespace tasksched
