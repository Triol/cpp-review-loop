#pragma once
// fcalc 内部文本工具（非公共面：仅供 src/ 下翻译单元包含，不进 include/）。
// 集中此前散落在 lexer.cpp / parser.cpp / evaluator.cpp 匿名命名空间中的
// 字符判定与文本处理助手，避免多副本实现漂移。

#include <cctype>
#include <cstddef>
#include <string>

namespace fcalc {

// 全库唯一空白集合：空格 / 水平制表 / 回车 / 换行（is_space 与 trim 共用）。
inline constexpr char kWhitespace[] = " \t\r\n";

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline bool is_space(char c) {
    for (const char* p = kWhitespace; *p != '\0'; ++p) {
        if (c == *p) return true;
    }
    return false;
}

inline std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(kWhitespace);
    if (first == std::string::npos) return "";
    const std::size_t last = s.find_last_not_of(kWhitespace);
    return s.substr(first, last - first + 1);
}

}  // namespace fcalc
