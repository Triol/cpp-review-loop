// config.cpp —— SchedulerConfig::loadFromFile / validate 的实现。

#include "config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>

namespace tasksched {
namespace {

/* 去掉字符串首尾空白 */
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/* 严格解析十进制整数：整个字符串必须都是合法数字，拒绝 "12abc" 之类 */
bool parseInteger(const std::string& text, long long* out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') return false;
    *out = value;
    return true;
}

} // namespace

bool SchedulerConfig::loadFromFile(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "无法打开配置文件: " + path;
        return false;
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#') continue; // 空行或注释

        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            if (error) *error += "配置第 " + std::to_string(lineNo) + " 行缺少 '='，已跳过\n";
            continue;
        }

        const std::string key = trim(entry.substr(0, eq));
        const std::string value = trim(entry.substr(eq + 1));
        if (key.empty() || value.empty()) {
            if (error) *error += "配置第 " + std::to_string(lineNo) + " 行键或值为空，已跳过\n";
            continue;
        }

        long long number = 0;
        if (key == "worker_threads") {
            if (parseInteger(value, &number) && number >= 1 && number <= 256) {
                worker_threads = static_cast<int>(number);
            } else if (error) {
                *error += "worker_threads 取值非法（应为 1..256），保留默认值\n";
            }
        } else if (key == "queue_capacity") {
            if (parseInteger(value, &number) && number >= 1) {
                queue_capacity = static_cast<std::size_t>(number);
            } else if (error) {
                *error += "queue_capacity 取值非法（应为 >=1 的整数），保留默认值\n";
            }
        } else if (key == "log_directory") {
            log_directory = value;
        } else if (key == "log_file_prefix") {
            log_file_prefix = value;
        } else if (key == "persistence_enabled") {
            persistence_enabled = (value == "1" || value == "true" ||
                                   value == "yes" || value == "on");
        } else if (key == "trace_enabled") {
            trace_enabled = (value == "1" || value == "true" ||
                             value == "yes" || value == "on");
        } else {
            // 未知键：保持兼容，静默忽略（仅记录告警）
            if (error) *error += "忽略未知配置键: " + key + "\n";
        }
    }
    return true;
}

bool SchedulerConfig::validate(std::string* error) const {
    if (worker_threads < 1) {
        if (error) *error = "worker_threads 必须 >= 1\n";
        return false;
    }
    if (queue_capacity < 1) {
        if (error) *error = "queue_capacity 必须 >= 1\n";
        return false;
    }
    if (persistence_enabled && log_directory.empty()) {
        if (error) *error = "启用持久化时 log_directory 不能为空\n";
        return false;
    }
    if (persistence_enabled && log_file_prefix.empty()) {
        if (error) *error = "启用持久化时 log_file_prefix 不能为空\n";
        return false;
    }
    return true;
}

} // namespace tasksched
