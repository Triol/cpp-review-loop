#ifndef TASKSCHED_PERSISTENCE_H
#define TASKSCHED_PERSISTENCE_H

/*
 * persistence.h —— legacy 持久化模块（C 接口）。
 *
 * 职责：把任务执行结果以文本行追加写入日志文件，文件名按天滚动：
 *     <log_dir>/<prefix>_YYYYMMDD.log
 * 每行格式：
 *     "YYYY-MM-DD HH:MM:SS task_id=<id> status=<OK|FAILED|RETRYING|CANCELLED>
 *      tag=<标签或-> type=<oneshot|periodic|-> worker=<0..N-1或->
 *      duration_us=<微秒整数或-> detail=<附加信息或->"
 *     （RETRYING 表示该次尝试失败但仍会重试；FAILED 表示最终失败。
 *       type 为任务类型：oneshot 一次性 / periodic 周期性；
 *       worker 为执行本任务的工作线程编号（0..N-1），未执行（如取消）或
 *       经旧接口写入的行记为 "-"；
 *       duration_us 为本次执行耗时，整数微秒（由旧版毫秒小数字段
 *       duration_ms 更名而来），未执行记为 "-"。
 *       旧格式行没有 worker / type 字段、耗时字段名为 duration_ms；
 *       统计侧按字段名取值，新旧格式天然兼容。）
 *
 * 历史说明：本模块移植自十年前的 C 项目，按团队约定保持 C 风格实现
 * 与 C ABI，供 C++ 侧的其他模块直接调用。所有资源（结构体、FILE*）
 * 均由 result_writer_open / result_writer_close 手动管理。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄，定义见 persistence.cpp */
typedef struct result_writer result_writer_t;

/*
 * 创建结果写入器，并确保日志目录存在（仅支持单级目录，
 * 逐级创建由调用方自行保证）。
 * 参数：
 *   log_dir : 日志目录路径，非空
 *   prefix  : 日志文件名前缀，非空（如 "task_results"）
 * 返回：句柄；失败返回 NULL（errno 指示原因）。
 */
result_writer_t *result_writer_open(const char *log_dir, const char *prefix);

/*
 * 追加一条任务执行结果（线程安全，内部有临界区；逐行 fflush）。
 * 兼容旧调用的便捷接口：等价于 result_writer_append_ex(..., tag=NULL, ...)。
 * 参数：
 *   task_id     : 任务 ID
 *   status      : "OK" / "FAILED" / "RETRYING" / "CANCELLED"
 *   duration_ms : 执行耗时（毫秒）；未执行（如取消）传负数，输出 "-"
 *                 （落盘时统一换算为整数微秒，写入 duration_us 字段）
 *   detail      : 附加信息，可为 NULL 或空串（输出 "-"）
 * 返回：0 成功；-1 失败（写失败时会关闭句柄，下次调用自动重开重试）。
 */
int result_writer_append(result_writer_t *writer, long long task_id,
                         const char *status, double duration_ms,
                         const char *detail);

/*
 * 扩展版追加接口：在 append 基础上多记录一个 tag 字段（任务标签，
 * 由 C++ 侧调度器随执行结果写入；旧调用方无需感知此新增字段；
 * 本接口写入的行 worker 字段记为 "-"）。
 * 参数：
 *   tag         : 任务标签，可为 NULL 或空串（输出 "-"）
 * 其余参数与返回值语义同 result_writer_append。
 */
int result_writer_append_ex(result_writer_t *writer, long long task_id,
                            const char *status, double duration_ms,
                            const char *tag, const char *detail);

/*
 * 扩展版追加接口（第 2 版）：在 append_ex 基础上多记录一个 type 字段
 * （任务类型 oneshot/periodic，由 C++ 侧调度器随执行结果写入；
 * 旧调用方无需感知此新增字段，旧接口内部转为本接口、type 记为 "-"；
 * 本接口写入的行 worker 字段记为 "-"）。
 * 参数：
 *   type        : 任务类型字符串（"oneshot"/"periodic"），可为 NULL 或空串
 *                 （输出 "-"，兼容不含类型概念的旧日志行）
 * 其余参数与返回值语义同 result_writer_append_ex。
 */
int result_writer_append_ex2(result_writer_t *writer, long long task_id,
                             const char *status, double duration_ms,
                             const char *tag, const char *type, const char *detail);

/*
 * 扩展版追加接口（第 3 版）：在 append_ex2 基础上，耗时参数改为整数微秒
 * （与日志行 duration_us 字段直接对应），并新增 worker 字段（执行本任务的
 * 工作线程编号，由 C++ 侧调度器随执行结果写入；旧调用方无需感知此新增
 * 字段，旧接口内部转为本接口、worker 记为 "-"）。
 * 参数：
 *   duration_us : 执行耗时（微秒，整数）；未执行（如取消）传负数，输出 "-"
 *   worker_id   : 工作线程编号（0..N-1）；无对应工作线程（如取消、旧接口）
 *                 传负数，输出 "-"
 * 其余参数与返回值语义同 result_writer_append_ex2。
 */
int result_writer_append_ex3(result_writer_t *writer, long long task_id,
                             const char *status, long long duration_us,
                             const char *tag, const char *type,
                             int worker_id, const char *detail);

/* 把未落盘的缓冲内容刷到磁盘。返回 0 成功；-1 失败。 */
int result_writer_flush(result_writer_t *writer);

/* 关闭写入器并释放全部资源；传入 NULL 安全。调用后句柄不可再用。 */
void result_writer_close(result_writer_t *writer);

#ifdef __cplusplus
}
#endif

#endif /* TASKSCHED_PERSISTENCE_H */
