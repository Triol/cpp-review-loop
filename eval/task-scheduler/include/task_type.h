#pragma once
// task_type.h —— 任务类型枚举（一次性 / 周期性）。
//
// 说明：与 priority.h 同理，是一个无依赖的最小公共头，供调度器、
// 持久化与统计模块共同包含。

namespace tasksched {

/* 任务执行模型：
 *   OneShot  一次性任务：执行一次（含重试）后即结束；
 *   Periodic 周期性任务：提交时可指定，成功执行完自动重新入队
 *            （不消耗重试额度；停机后不再重新入队，保证 shutdown 可排空）。 */
enum class TaskType : int {
    OneShot = 0,
    Periodic = 1,
};

/* 任务类型的规范拼写（落盘日志行 type 字段的取值）。 */
inline const char* taskTypeName(TaskType type) noexcept {
    return (type == TaskType::Periodic) ? "periodic" : "oneshot";
}

} // namespace tasksched
