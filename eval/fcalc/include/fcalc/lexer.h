#pragma once
// fcalc 词法分析：把公式源文本切分为 Token 流。

#include <string>

namespace fcalc {

enum class TokenKind {
    Number, String, Identifier,
    Plus, Minus, Star, Slash, Caret, Percent, Amp,
    Eq, NotEq, Less, LessEq, Greater, GreaterEq,
    LParen, RParen, Comma, Colon,
    End,
    Invalid,  // 非法字符 / 未终止的字符串
};

struct Token {
    TokenKind kind = TokenKind::Invalid;
    std::string text;    // 原始词文；String 时为解码后的内容
    double number = 0.0; // kind == Number 时的数值
    int pos = 0;         // 源文本中的起始偏移
};

// 逐 Token 推进：peek() 查看当前 Token，advance() 消费并扫描下一个。
class Lexer {
public:
    explicit Lexer(std::string src);

    const Token& peek() const { return current_; }
    void advance();

private:
    void scan();
    void scan_number();
    void scan_identifier();
    void scan_string();
    void scan_operator();

    std::string src_;
    std::size_t offset_ = 0;
    Token current_;
};

}  // namespace fcalc
