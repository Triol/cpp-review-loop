#include "fcalc/lexer.h"

#include "text_util.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace fcalc {

Lexer::Lexer(std::string src) : src_(std::move(src)) { scan(); }

void Lexer::advance() { scan(); }

void Lexer::scan() {
    while (offset_ < src_.size() && is_space(src_[offset_])) ++offset_;

    if (offset_ >= src_.size()) {
        current_ = Token{TokenKind::End, "", 0.0, static_cast<int>(offset_)};
        return;
    }

    const char c = src_[offset_];
    if (is_digit(c) || (c == '.' && offset_ + 1 < src_.size() && is_digit(src_[offset_ + 1]))) {
        scan_number();
    } else if (is_letter(c) || (c == '$' && offset_ + 1 < src_.size() &&
                                is_letter(src_[offset_ + 1]))) {
        // '$' 后跟字母 → 绝对引用的起始（如 $A$1）；其余位置仍按非法字符处理
        scan_identifier();
    } else if (c == '"') {
        scan_string();
    } else {
        scan_operator();
    }
}

// 数字：[0-9]+ ('.' [0-9]*)? | '.' [0-9]+，可选指数部分（1e3 / 2.5E-2）。
void Lexer::scan_number() {
    const std::size_t start = offset_;
    while (offset_ < src_.size() && is_digit(src_[offset_])) ++offset_;
    if (offset_ < src_.size() && src_[offset_] == '.') {
        ++offset_;
        while (offset_ < src_.size() && is_digit(src_[offset_])) ++offset_;
    }
    if (offset_ < src_.size() && (src_[offset_] == 'e' || src_[offset_] == 'E')) {
        std::size_t look = offset_ + 1;
        if (look < src_.size() && (src_[look] == '+' || src_[look] == '-')) ++look;
        if (look < src_.size() && is_digit(src_[look])) {
            offset_ = look;
            while (offset_ < src_.size() && is_digit(src_[offset_])) ++offset_;
        }
    }
    const std::string text = src_.substr(start, offset_ - start);
    errno = 0;
    const double value = std::strtod(text.c_str(), nullptr);
    if (errno == ERANGE || !std::isfinite(value)) {
        // 数值字面量溢出（如 1e999 → inf）：产出 Invalid token，走既有语法错误路径
        // 返回 #VALUE!，不绕过全库『#VALUE! = 数值溢出』约定。
        current_ = Token{TokenKind::Invalid, "numeric literal overflow", 0.0,
                         static_cast<int>(start)};
        return;
    }
    current_ = Token{TokenKind::Number, text, value, static_cast<int>(start)};
}

// 标识符：字母或 "$字母" 开头的字母数字串，内部可含 '$'（绝对引用标记，如 $A$1 / A$1）；
// 后续由语法层分类为布尔 / 单元格引用 / 未知名字。
void Lexer::scan_identifier() {
    const std::size_t start = offset_;
    ++offset_;  // 首字符必为字母或 '$'
    while (offset_ < src_.size() &&
           (is_letter(src_[offset_]) || is_digit(src_[offset_]) || src_[offset_] == '$')) {
        ++offset_;
    }
    current_ = Token{TokenKind::Identifier, src_.substr(start, offset_ - start), 0.0,
                     static_cast<int>(start)};
}

// 字符串字面量：双引号包围，"" 转义为字面引号；未闭合 → Invalid。
void Lexer::scan_string() {
    const std::size_t start = offset_;
    ++offset_;  // 跳过开头引号
    std::string decoded;
    bool closed = false;
    while (offset_ < src_.size()) {
        if (src_[offset_] != '"') {
            decoded += src_[offset_++];
            continue;
        }
        if (offset_ + 1 < src_.size() && src_[offset_ + 1] == '"') {
            decoded += '"';  // "" → 字面引号
            offset_ += 2;
            continue;
        }
        ++offset_;  // 结束引号
        closed = true;
        break;
    }
    if (closed) {
        current_ = Token{TokenKind::String, std::move(decoded), 0.0, static_cast<int>(start)};
    } else {
        current_ = Token{TokenKind::Invalid, "unterminated string", 0.0, static_cast<int>(start)};
    }
}

// 运算符与标点（含两字符的 <= >= <>）。
void Lexer::scan_operator() {
    const std::size_t start = offset_;
    const char c = src_[offset_++];
    TokenKind kind = TokenKind::Invalid;
    std::size_t len = 1;
    switch (c) {
        case '+': kind = TokenKind::Plus; break;
        case '-': kind = TokenKind::Minus; break;
        case '*': kind = TokenKind::Star; break;
        case '/': kind = TokenKind::Slash; break;
        case '^': kind = TokenKind::Caret; break;
        case '%': kind = TokenKind::Percent; break;
        case '&': kind = TokenKind::Amp; break;
        case '(': kind = TokenKind::LParen; break;
        case ')': kind = TokenKind::RParen; break;
        case ',': kind = TokenKind::Comma; break;   // 函数实参分隔符
        case ':': kind = TokenKind::Colon; break;   // 范围分隔符（A1:B3）
        case '=': kind = TokenKind::Eq; break;
        case '<':
            if (offset_ < src_.size() && src_[offset_] == '=') {
                kind = TokenKind::LessEq;
                ++offset_;
                ++len;
            } else if (offset_ < src_.size() && src_[offset_] == '>') {
                kind = TokenKind::NotEq;
                ++offset_;
                ++len;
            } else {
                kind = TokenKind::Less;
            }
            break;
        case '>':
            if (offset_ < src_.size() && src_[offset_] == '=') {
                kind = TokenKind::GreaterEq;
                ++offset_;
                ++len;
            } else {
                kind = TokenKind::Greater;
            }
            break;
        default: kind = TokenKind::Invalid; break;
    }
    current_ = Token{kind, src_.substr(start, len), 0.0, static_cast<int>(start)};
}

}  // namespace fcalc
