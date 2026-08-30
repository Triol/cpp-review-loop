#pragma once
// priority.h —— 任务优先级枚举。
//
// 说明：原定义在 task_scheduler.h 中；本轮为让 Metrics 记录“按优先级的
// 成功执行计数”，把枚举下沉到这个无依赖的最小公共头，供调度器与
// metrics 共同包含（包含 task_scheduler.h 的既有代码不受影响）。

namespace tasksched {

/* 任务优先级：数值越大越先出队；同优先级内保持提交顺序（FIFO）。 */
enum class Priority : int {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

/* 优先级的规范拼写（追踪日志 priority 字段的取值，全小写）。 */
inline const char* priorityName(Priority priority) noexcept {
    switch (priority) {
        case Priority::Low:      return "low";
        case Priority::High:     return "high";
        case Priority::Critical: return "critical";
        default:                 return "normal";
    }
}

} // namespace tasksched
