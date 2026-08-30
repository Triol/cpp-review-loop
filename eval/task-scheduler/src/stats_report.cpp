// stats_report.cpp —— 统计报表模块实现：解析当天任务日志，按 tag 汇总
// 各状态条数与 p95 执行耗时，支持文本表格 / JSON / CSV 三种渲染。

#include "stats_report.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace tasksched {
namespace {

/* 与持久化模块一致的当天日期戳 "YYYYMMDD"（本地时间，同一套按天滚动命名规则） */
std::string todayStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[16] = {0};
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv);
    return std::string(buf);
}

/*
 * 解析 worker 编号（非负十进制整数，0..N-1）。
 * 字段缺失 / "-" / 含非数字字符时返回 false（*worker 保持 -1），
 * 对应旧格式行与未经过 worker 执行的行（如 CANCELLED）。
 */
bool parseWorkerNumber(const std::string& value, int* worker) {
    if (value.empty() || value.size() > 9) return false; /* 位数上限防溢出 */
    long long number = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        number = number * 10 + (ch - '0');
    }
    *worker = static_cast<int>(number);
    return true;
}

/*
 * 解析无符号十进制整数（用于耗时字段，单位微秒）；上限 18 位防溢出 uint64。
 * 字段缺失（"-"）或含非法字符时返回 false。
 */
bool parseUnsignedNumber(const std::string& value, std::uint64_t* number) {
    if (value.empty() || value.size() > 18) return false;
    std::uint64_t result = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        result = result * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    *number = result;
    return true;
}

/*
 * 解析旧格式 duration_ms 字段（"%.3f" 形式的毫秒小数，如 "12.345"、"5.5"），
 * 换算成整数微秒——与持久化模块把毫秒落盘为 duration_us 的方向一致。
 * 整数部分上限 15 位，保证乘 1000 换算微秒后不溢出。
 */
bool parseMsFieldToUs(const std::string& value, std::uint64_t* us) {
    const std::size_t dot = value.find('.');
    const std::string intPart =
        (dot == std::string::npos) ? value : value.substr(0, dot);
    std::string fracPart =
        (dot == std::string::npos) ? std::string() : value.substr(dot + 1);
    if (intPart.empty() || intPart.size() > 15 || fracPart.size() > 3) return false;
    std::uint64_t ms = 0;
    if (!parseUnsignedNumber(intPart, &ms)) return false;
    std::uint64_t fracUs = 0;
    if (!fracPart.empty()) {
        while (fracPart.size() < 3) fracPart.push_back('0'); /* "5" -> 500 微秒 */
        if (!parseUnsignedNumber(fracPart, &fracUs)) return false;
    }
    *us = ms * 1000 + fracUs;
    return true;
}

/*
 * 从一行日志中提取 status / tag / worker / duration 字段。
 * 只扫描 "detail=" 之前的 token：detail 是自由文本（可含空格），
 * 其后出现的 "tag=" 之类字样不可信。
 * worker 未出现在行内或取值非法时 *worker 保持 -1。
 * 耗时字段：新格式 duration_us（整数微秒）优先；旧格式 duration_ms
 * （毫秒小数）换算成微秒兜底。两者都无效时 *hasDuration 为 false。
 * 返回 true 表示找到了 status 字段（值写入 *status，可能是未知状态）。
 */
bool parseLine(const std::string& line, std::string* status, std::string* tag,
               int* worker, std::uint64_t* durationUs, bool* hasDuration) {
    std::istringstream in(line);
    std::string token;
    bool hasStatus = false;
    bool hasTag = false;
    bool hasUs = false; /* 已解析到新格式 duration_us（整数微秒，优先采用） */
    bool hasMs = false; /* 已解析到旧格式 duration_ms（换算成微秒，兜底） */
    std::uint64_t us = 0;
    std::uint64_t msUs = 0;
    while (in >> token) {
        if (token.rfind("detail=", 0) == 0) break;
        if (!hasStatus && token.rfind("status=", 0) == 0) {
            *status = token.substr(7);
            hasStatus = true;
        } else if (!hasTag && token.rfind("tag=", 0) == 0) {
            *tag = token.substr(4);
            hasTag = true;
        } else if (*worker < 0 && token.rfind("worker=", 0) == 0) {
            parseWorkerNumber(token.substr(7), worker);
        } else if (!hasUs && token.rfind("duration_us=", 0) == 0) {
            if (parseUnsignedNumber(token.substr(12), &us)) hasUs = true;
        } else if (!hasMs && token.rfind("duration_ms=", 0) == 0) {
            if (parseMsFieldToUs(token.substr(12), &msUs)) hasMs = true;
        }
    }
    if (hasUs) {
        *durationUs = us;
        *hasDuration = true;
    } else if (hasMs) {
        *durationUs = msUs;
        *hasDuration = true;
    } else {
        *hasDuration = false;
    }
    return hasStatus;
}

/* 把一条已识别的行计入对应状态的计数槽。 */
void addStatus(TagStats& stats, const std::string& status) {
    if (status == "OK") {
        ++stats.ok;
    } else if (status == "FAILED") {
        ++stats.failed;
    } else if (status == "RETRYING") {
        ++stats.retrying;
    } else {
        ++stats.cancelled;
    }
}

/*
 * 取样本的 95 分位执行耗时并换算成毫秒：最近秩（nearest-rank）法——
 * 升序排序后取第 ceil(0.95 * n) 个样本（1 基）。样本为整数微秒
 * （按值传入，排序不改动调用方数据）；调用方保证样本非空。
 */
double p95DurationMs(std::vector<std::uint64_t> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t rank = (samples.size() * 95 + 99) / 100; /* ceil(n * 0.95) */
    return static_cast<double>(samples[rank - 1]) / 1000.0;
}

/* 毫秒值 -> 固定 3 位小数的文本（文本 / JSON / CSV 三种输出共用同一精度） */
std::string formatMs(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return std::string(buf);
}

/* CSV 字段编码：含逗号 / 引号 / 回车 / 换行的字段加引号、内部引号翻倍，
   其余原样输出。 */
std::string csvField(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

/* 把字符串编码成带引号的 JSON 字符串字面量（转义引号、反斜杠与控制字符） */
std::string jsonQuoted(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out << buf;
            } else {
                out << ch; // UTF-8 字节原样透传，JSON 合法
            }
        }
    }
    out << '"';
    return out.str();
}

} // namespace

DailyTagStatsReport buildDailyTagStats(const std::string& log_dir,
                                       const std::string& prefix) {
    DailyTagStatsReport report;
    report.log_file = log_dir + "/" + prefix + "_" + todayStamp() + ".log";

    std::ifstream in(report.log_file);
    if (!in.is_open()) {
        return report; /* 文件不存在（今天还没有任务落盘）：空报表，不算错误 */
    }
    report.file_found = true;

    /* 每个 tag 的执行耗时样本（微秒）：读完全部行后统一算 95 分位 */
    std::map<std::string, std::vector<std::uint64_t>> durationSamples;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); /* 兼容 CRLF */
        if (line.empty()) continue;
        ++report.lines_total;

        std::string status;
        std::string tag;
        int worker = -1;
        std::uint64_t durationUs = 0;
        bool hasDuration = false;
        if (!parseLine(line, &status, &tag, &worker, &durationUs, &hasDuration)) {
            ++report.lines_skipped; /* 缺少状态字段（残缺行 / 非任务日志行） */
            continue;
        }
        if (status != "OK" && status != "FAILED" &&
            status != "RETRYING" && status != "CANCELLED") {
            ++report.lines_skipped; /* 状态值无法识别 */
            continue;
        }

        const std::string tagKey = tag.empty() ? std::string("-") : tag;
        TagStats& slot = report.by_tag[tagKey];
        ++report.lines_parsed;
        addStatus(slot, status);
        /* 按 worker 分组：只统计真正在某个 worker 上执行过的尝试
           （重试尝试计入当时执行它的 worker；CANCELLED 行没有 worker）。 */
        if (worker >= 0) {
            addStatus(report.by_worker[worker], status);
        }
        /* p95 样本：所有带耗时字段的已识别行都参与，不要求 worker 字段
           （旧格式 duration_ms 行已在 parseLine 里换算成微秒）；
           CANCELLED 行的耗时字段是 "-"，天然不参与。 */
        if (hasDuration) {
            durationSamples[tagKey].push_back(durationUs);
        }
    }

    for (const auto& entry : durationSamples) {
        report.p95_ms_by_tag[entry.first] = p95DurationMs(entry.second);
    }
    return report;
}

TagStats DailyTagStatsReport::total() const noexcept {
    TagStats sum;
    for (const auto& entry : by_tag) {
        sum.ok += entry.second.ok;
        sum.failed += entry.second.failed;
        sum.retrying += entry.second.retrying;
        sum.cancelled += entry.second.cancelled;
    }
    return sum;
}

TagStats DailyTagStatsReport::workerTotal() const noexcept {
    TagStats sum;
    for (const auto& entry : by_worker) {
        sum.ok += entry.second.ok;
        sum.failed += entry.second.failed;
        sum.retrying += entry.second.retrying;
        sum.cancelled += entry.second.cancelled;
    }
    return sum;
}

std::string dailyTagStatsToText(const DailyTagStatsReport& report) {
    std::ostringstream out;
    const char* const noP95 = "-"; /* 该 tag 没有任何带耗时的行时 p95 列显示 "-" */

    out << "[stats] 任务日志报表: " << report.log_file << "\n";
    if (!report.file_found) {
        out << "[stats] 日志文件不存在，今天尚无落盘记录\n";
    } else {
        out << "[stats] 行数: 总计 " << report.lines_total
            << ", 已统计 " << report.lines_parsed
            << ", 跳过 " << report.lines_skipped << "\n";

        /* tag 列宽自适应：不截断内容，只保证各列对齐 */
        std::size_t width = 8;
        for (const auto& entry : report.by_tag) {
            width = std::max(width, entry.first.size());
        }
        out << "[stats] " << std::left << std::setw(static_cast<int>(width)) << "tag"
            << " | " << std::right << std::setw(8) << "OK"
            << " | " << std::setw(8) << "FAILED"
            << " | " << std::setw(8) << "RETRYING"
            << " | " << std::setw(9) << "CANCELLED"
            << " | " << std::setw(5) << "TOTAL"
            << " | " << std::setw(10) << "P95(ms)" << "\n";
        for (const auto& entry : report.by_tag) {
            const TagStats& st = entry.second;
            const auto p95It = report.p95_ms_by_tag.find(entry.first);
            out << "[stats] " << std::left << std::setw(static_cast<int>(width))
                << entry.first
                << " | " << std::right << std::setw(8) << st.ok
                << " | " << std::setw(8) << st.failed
                << " | " << std::setw(8) << st.retrying
                << " | " << std::setw(9) << st.cancelled
                << " | " << std::setw(5) << st.total()
                << " | " << std::setw(10)
                << (p95It != report.p95_ms_by_tag.end() ? formatMs(p95It->second)
                                                        : std::string(noP95))
                << "\n";
        }
        const TagStats sum = report.total();
        out << "[stats] " << std::left << std::setw(static_cast<int>(width)) << "TOTAL"
            << " | " << std::right << std::setw(8) << sum.ok
            << " | " << std::setw(8) << sum.failed
            << " | " << std::setw(8) << sum.retrying
            << " | " << std::setw(9) << sum.cancelled
            << " | " << std::setw(5) << sum.total()
            << " | " << std::setw(10) << noP95 << "\n";

        /* 按 worker 编号分组（编号升序）；旧格式行 / 取消行没有 worker 字段，
           不参与本表，因此各行之和可能小于上面的 TOTAL。 */
        if (!report.by_worker.empty()) {
            out << "[stats] 按 worker 分组（仅统计带 worker 编号的执行行）:\n";
            out << "[stats] " << std::right << std::setw(6) << "worker"
                << " | " << std::setw(8) << "OK"
                << " | " << std::setw(8) << "FAILED"
                << " | " << std::setw(8) << "RETRYING"
                << " | " << std::setw(9) << "CANCELLED"
                << " | " << std::setw(5) << "TOTAL" << "\n";
            for (const auto& entry : report.by_worker) {
                const TagStats& st = entry.second;
                out << "[stats] " << std::right << std::setw(6) << entry.first
                    << " | " << std::setw(8) << st.ok
                    << " | " << std::setw(8) << st.failed
                    << " | " << std::setw(8) << st.retrying
                    << " | " << std::setw(9) << st.cancelled
                    << " | " << std::setw(5) << st.total() << "\n";
            }
            const TagStats workerSum = report.workerTotal();
            out << "[stats] " << std::right << std::setw(6) << "TOTAL"
                << " | " << std::setw(8) << workerSum.ok
                << " | " << std::setw(8) << workerSum.failed
                << " | " << std::setw(8) << workerSum.retrying
                << " | " << std::setw(9) << workerSum.cancelled
                << " | " << std::setw(5) << workerSum.total() << "\n";
        }
    }

    /* 结尾换行由调用方统一补，便于三种输出形态走同一条打印路径 */
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

void printDailyTagStats(const DailyTagStatsReport& report) {
    std::string text = dailyTagStatsToText(report);
    text.push_back('\n');
    std::fwrite(text.data(), 1, text.size(), stdout);
}

std::string dailyTagStatsToJson(const DailyTagStatsReport& report) {
    const TagStats sum = report.total();
    std::ostringstream out;
    out << "{\n";
    out << "  \"log_file\": " << jsonQuoted(report.log_file) << ",\n";
    out << "  \"file_found\": " << (report.file_found ? "true" : "false") << ",\n";
    out << "  \"lines_total\": " << report.lines_total << ",\n";
    out << "  \"lines_parsed\": " << report.lines_parsed << ",\n";
    out << "  \"lines_skipped\": " << report.lines_skipped << ",\n";
    out << "  \"tags\": [";
    bool first = true;
    for (const auto& entry : report.by_tag) {
        const TagStats& st = entry.second;
        out << (first ? "\n" : ",\n");
        first = false;
        const auto p95It = report.p95_ms_by_tag.find(entry.first);
        out << "    {\"tag\": " << jsonQuoted(entry.first)
            << ", \"ok\": " << st.ok
            << ", \"failed\": " << st.failed
            << ", \"retrying\": " << st.retrying
            << ", \"cancelled\": " << st.cancelled
            << ", \"total\": " << st.total()
            << ", \"p95_duration_ms\": ";
        if (p95It != report.p95_ms_by_tag.end()) {
            out << formatMs(p95It->second); /* 数字，固定 3 位小数 */
        } else {
            out << "null"; /* 该 tag 没有任何带耗时的行 */
        }
        out << "}";
    }
    out << (first ? "],\n" : "\n  ],\n");
    out << "  \"workers\": [";
    bool firstWorker = true;
    for (const auto& entry : report.by_worker) {
        const TagStats& st = entry.second;
        out << (firstWorker ? "\n" : ",\n");
        firstWorker = false;
        out << "    {\"worker\": " << entry.first
            << ", \"ok\": " << st.ok
            << ", \"failed\": " << st.failed
            << ", \"retrying\": " << st.retrying
            << ", \"cancelled\": " << st.cancelled
            << ", \"total\": " << st.total() << "}";
    }
    out << (firstWorker ? "],\n" : "\n  ],\n");
    out << "  \"total\": {\"ok\": " << sum.ok
        << ", \"failed\": " << sum.failed
        << ", \"retrying\": " << sum.retrying
        << ", \"cancelled\": " << sum.cancelled
        << ", \"total\": " << sum.total() << "}\n";
    out << "}";
    return out.str();
}

std::string dailyTagStatsToCsv(const DailyTagStatsReport& report) {
    std::ostringstream out;

    /* 元数据行（key,value 两列，key 名与 JSON 顶层字段一致） */
    out << csvField("log_file") << "," << csvField(report.log_file) << "\n";
    out << "file_found," << (report.file_found ? "true" : "false") << "\n";
    out << "lines_total," << report.lines_total << "\n";
    out << "lines_parsed," << report.lines_parsed << "\n";
    out << "lines_skipped," << report.lines_skipped << "\n";

    /* tags 表：字段名与 JSON tags 条目一致，末行 TOTAL 的 p95 列留空
       （合计行的 95 分位不参与统计，对应 JSON total 对象无此字段） */
    out << "tag,ok,failed,retrying,cancelled,total,p95_duration_ms\n";
    for (const auto& entry : report.by_tag) {
        const TagStats& st = entry.second;
        const auto p95It = report.p95_ms_by_tag.find(entry.first);
        out << csvField(entry.first)
            << "," << st.ok
            << "," << st.failed
            << "," << st.retrying
            << "," << st.cancelled
            << "," << st.total()
            << ",";
        if (p95It != report.p95_ms_by_tag.end()) {
            out << formatMs(p95It->second); /* 留空对应 JSON 里的 null */
        }
        out << "\n";
    }
    const TagStats sum = report.total();
    out << "TOTAL," << sum.ok << "," << sum.failed << "," << sum.retrying
        << "," << sum.cancelled << "," << sum.total() << ",\n";

    /* workers 表：字段名与 JSON workers 条目一致 */
    out << "worker,ok,failed,retrying,cancelled,total\n";
    for (const auto& entry : report.by_worker) {
        const TagStats& st = entry.second;
        out << entry.first
            << "," << st.ok
            << "," << st.failed
            << "," << st.retrying
            << "," << st.cancelled
            << "," << st.total() << "\n";
    }
    const TagStats workerSum = report.workerTotal();
    out << "TOTAL," << workerSum.ok << "," << workerSum.failed << ","
        << workerSum.retrying << "," << workerSum.cancelled << ","
        << workerSum.total() << "\n";

    return out.str();
}

} // namespace tasksched
