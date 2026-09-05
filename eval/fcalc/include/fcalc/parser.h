#pragma once
// fcalc 语法分析：递归下降 → 表达式树。
//
// 优先级（低 → 高）：
//   比较 = <> < <= > >=   <   &（连接）  <   + -   <   * /   <   ^（右结合）
//   <   一元 - +   <   后缀 %   <   字面量 / 引用 / 范围 A1:B3 / 函数调用 FN(...) / 括号
// 注：一元负号比 ^ 结合更紧（-2^2 = (-2)^2 = 4，同 Excel），
//     且 ^ 的指数侧允许一元符号（2^-3 = 0.25）。

#include <memory>
#include <string>
#include <vector>

#include "fcalc/error_factory.h"
#include "fcalc/lexer.h"

namespace fcalc {

// 表达式树节点：带标签的紧凑结构（字面量 / 引用 / 范围 / 函数调用 + 一元 / 二元节点）。
struct Expr {
    enum class Kind { Number, String, Bool, Ref, Range, Call, Unary, Binary };

    Kind kind = Kind::Number;
    double number = 0.0;                // Number
    std::string text;                   // String 内容 / 规范化的单元格引用（如 "A1"）/
                                        // Range 起始引用 / Call 函数名（规范化大写）
    std::string text_end;               // Range: 结束单元格引用（如 "B3"）
    bool boolean = false;               // Bool
    TokenKind op = TokenKind::Invalid;  // Unary: Minus/Plus/Percent；Binary: 其余运算符
    std::vector<std::unique_ptr<Expr>> args;  // Call: 逗号分隔的实参列表
    std::unique_ptr<Expr> lhs;          // Unary / Binary 的左（或唯一）操作数
    std::unique_ptr<Expr> rhs;          // Binary 的右操作数
};

// 解析结果：ok == false 时 error 为应返回给调用方的错误值。
struct ParseResult {
    std::unique_ptr<Expr> expr;
    Error error{ErrorType::Value};
    bool ok = false;
};

// 解析以 '=' 开头的完整公式；缺少 '=' 前缀 → #VALUE!。
ParseResult parse_formula(const std::string& text);

// 递归下降解析器（持有 Lexer，逐层下降并组装 Expr 树）。
class Parser {
public:
    explicit Parser(Lexer lexer);
    ParseResult parse();

private:
    std::unique_ptr<Expr> parse_expr();
    std::unique_ptr<Expr> parse_comparison();
    std::unique_ptr<Expr> parse_concat();
    std::unique_ptr<Expr> parse_additive();
    std::unique_ptr<Expr> parse_multiplicative();
    std::unique_ptr<Expr> parse_power();
    std::unique_ptr<Expr> parse_unary();
    std::unique_ptr<Expr> parse_postfix();
    std::unique_ptr<Expr> parse_primary();
    std::unique_ptr<Expr> parse_call_tail(std::string name);  // '(' 后的实参列表与 ')'

    const Token& peek() const { return lexer_.peek(); }
    void advance() { lexer_.advance(); }
    void fail(Error e) { error_ = e; }

    Lexer lexer_;
    Error error_{ErrorType::Value};
    int depth_ = 0;  // 括号嵌套深度（parse_primary 的 LParen 分支维护，超限报 #VALUE!）
};

}  // namespace fcalc
