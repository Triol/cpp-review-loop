#pragma once
// stats_report.h —— 统计报表模块：读取"当天"的任务日志文件，按 tag 汇总
// 各状态（OK / FAILED / RETRYING / CANCELLED）的条数，并按 worker 编号
// （日志行 worker 字段，0..N-1）分组统计各自执行的条数；另为每个 tag 计算
// p95 执行耗时（毫秒）。支持文本表格 / JSON / CSV 三种输出形式。
//
// 日志文件定位规则与持久化模块保持一致（按天滚动）：
//     <log_dir>/<prefix>_YYYYMMDD.log
// 解析兼容性：只认行内 "status=" / "tag=" / "worker=" / "duration_us=" 字段
// （旧格式的 duration_ms 字段也认，换算成微秒后参与 p95 统计），因此旧格式
// 行（无 type / worker 字段）也能统计；无法识别的行计入 skipped，不参与汇总。

#include <cstdint>
#include <map>
#include <string>

namespace tasksched {

/* 单个 tag 的各状态条数。 */
struct TagStats {
    std::uint64_t ok = 0;        // status=OK 的行数
    std::uint64_t failed = 0;    // status=FAILED 的行数（最终失败）
    std::uint64_t retrying = 0;  // status=RETRYING 的行数（失败后仍会重试）
    std::uint64_t cancelled = 0; // status=CANCELLED 的行数

    std::uint64_t total() const noexcept { return ok + failed + retrying + cancelled; }
};

/* 一次统计的结果快照。 */
struct DailyTagStatsReport {
    std::string log_file;             // 实际读取的日志文件路径
    bool file_found = false;          // 日志文件是否存在且可打开
    std::uint64_t lines_total = 0;    // 文件中非空行的总数
    std::uint64_t lines_parsed = 0;   // 成功识别并计入统计的行数
    std::uint64_t lines_skipped = 0;  // 缺少状态字段 / 状态值未知的行数
    std::map<std::string, TagStats> by_tag; // 按 tag 分组（字典序）；无标签记为 "-"
    /* 按 worker 编号分组（编号升序）；只统计带 worker 字段的行——
       CANCELLED 行与旧格式行没有 worker 信息，不计入本分组。 */
    std::map<int, TagStats> by_worker;

    /* 每个 tag 的 p95 执行耗时（毫秒）。键为 by_tag 的子集：只含至少有
       一条带耗时记录的 tag。参与计算的样本：该 tag 全部带耗时字段的
       已识别行——不要求 worker 字段（新格式 duration_us 直接取值，旧格式
       duration_ms 换算成微秒后参与）；CANCELLED 行没有耗时，天然不参与。 */
    std::map<std::string, double> p95_ms_by_tag;

    /* 所有 tag 的合计。 */
    TagStats total() const noexcept;

    /* 所有 worker 的合计（= 带 worker 字段的行数）。 */
    TagStats workerTotal() const noexcept;
};

/*
 * 统计"今天"的任务日志（<log_dir>/<prefix>_YYYYMMDD.log，本地时间）。
 * 日志文件不存在时同样返回结果（file_found=false，各计数为 0），不算错误。
 */
DailyTagStatsReport buildDailyTagStats(const std::string& log_dir,
                                       const std::string& prefix);

/* 以对齐的文本表格把报表打印到 stdout（等价于输出 dailyTagStatsToText 的
   结果并补上结尾换行）。 */
void printDailyTagStats(const DailyTagStatsReport& report);

/* 把报表渲染成对齐的文本表格；返回的字符串不带结尾换行。 */
std::string dailyTagStatsToText(const DailyTagStatsReport& report);

/*
 * 把报表序列化成 JSON 字符串（key 名为英文，便于程序消费）：
 *   log_file / file_found / lines_total / lines_parsed / lines_skipped /
 *   tags: [{tag, ok, failed, retrying, cancelled, total, p95_duration_ms}...] /
 *   workers: [{worker, ok, failed, retrying, cancelled, total}...] /
 *   total: {ok, failed, retrying, cancelled, total}
 * （workers 只含带 worker 编号的行；各 worker 条目之和 <= total。
 *   p95_duration_ms 为该 tag 全部带耗时行耗时的 95 分位，单位毫秒；
 *   该 tag 没有任何带耗时的行时值为 null。）
 * 返回的字符串不带结尾换行，由调用方决定如何打印。
 */
std::string dailyTagStatsToJson(const DailyTagStatsReport& report);

/*
 * 把报表序列化成 CSV 字符串（含表头行，字段名与 JSON 一致）：
 *   元数据行（key,value 两列，key 名与 JSON 顶层字段一致）：
 *     log_file / file_found / lines_total / lines_parsed / lines_skipped
 *   tags 表：tag,ok,failed,retrying,cancelled,total,p95_duration_ms
 *     （每个 tag 一行，最后附 TOTAL 行；TOTAL 行的 p95 列留空）
 *   workers 表：worker,ok,failed,retrying,cancelled,total
 *     （每个 worker 一行，最后附 TOTAL 行）
 * 含逗号 / 引号 / 换行的字段按 CSV 规则加引号转义。
 * 返回的字符串不带结尾换行，由调用方决定如何打印。
 */
std::string dailyTagStatsToCsv(const DailyTagStatsReport& report);

} // namespace tasksched
