#include "fcalc/parser.h"

#include <cctype>

namespace fcalc {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// 单元格引用 A1..ZZ99：1-2 个列字母 + 1-2 位行号（1..99）。
// 匹配时输出规范化引用（大写列号 + 无前导零行号），如 "a01" → "A1"。
bool try_cell_ref(const std::string& id, std::string& out) {
    std::size_t i = 0;
    while (i < id.size() && is_letter(id[i])) ++i;
    const std::size_t letters = i;
    const std::size_t digits_start = i;
    while (i < id.size() && is_digit(id[i])) ++i;
    const std::size_t digits = i - digits_start;
    if (i != id.size() || letters == 0 || letters > 2 || digits == 0 || digits > 2) return false;
    int row = 0;
    for (std::size_t k = digits_start; k < i; ++k) row = row * 10 + (id[k] - '0');
    if (row < 1) return false;
    out = to_upper(id.substr(0, letters)) + std::to_string(row);
    return true;
}

std::unique_ptr<Expr> make_number(double v) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Number;
    e->number = v;
    return e;
}

std::unique_ptr<Expr> make_string(std::string s) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::String;
    e->text = std::move(s);
    return e;
}

std::unique_ptr<Expr> make_bool(bool b) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Bool;
    e->boolean = b;
    return e;
}

std::unique_ptr<Expr> make_ref(std::string ref) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Ref;
    e->text = std::move(ref);
    return e;
}

std::unique_ptr<Expr> make_range(std::string start, std::string end) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Range;
    e->text = std::move(start);
    e->text_end = std::move(end);
    return e;
}

std::unique_ptr<Expr> make_call(std::string name) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Call;
    e->text = std::move(name);  // 规范化为大写：函数名大小写不敏感
    return e;
}

std::unique_ptr<Expr> make_unary(TokenKind op, std::unique_ptr<Expr> operand) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Unary;
    e->op = op;
    e->lhs = std::move(operand);
    return e;
}

std::unique_ptr<Expr> make_binary(TokenKind op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Binary;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    return e;
}

bool is_comparison(TokenKind k) {
    switch (k) {
        case TokenKind::Eq:
        case TokenKind::NotEq:
        case TokenKind::Less:
        case TokenKind::LessEq:
        case TokenKind::Greater:
        case TokenKind::GreaterEq:
            return true;
        default:
            return false;
    }
}

}  // namespace

ParseResult parse_formula(const std::string& text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || text[start] != '=') {
        ParseResult r;  // 公式必须以 '=' 开头
        r.error = ErrorFactory::value();
        return r;
    }
    Parser parser{Lexer(text)};  // 花括号避免最令人头疼的解析（most vexing parse）
    return parser.parse();
}

Parser::Parser(Lexer lexer) : lexer_(std::move(lexer)) {}

ParseResult Parser::parse() {
    if (peek().kind != TokenKind::Eq) {  // parse_formula 已保证；防御性检查
        ParseResult r;
        r.error = ErrorFactory::value();
        return r;
    }
    advance();

    std::unique_ptr<Expr> expr = parse_expr();
    if (!expr) {
        ParseResult r;
        r.error = error_;
        return r;
    }
    if (peek().kind != TokenKind::End) {  // 存在未消费的尾随内容 → 语法错误
        ParseResult r;
        r.error = ErrorFactory::value();
        return r;
    }
    ParseResult r;
    r.expr = std::move(expr);
    r.ok = true;
    return r;
}

// 各优先级层级：comparison → concat → additive → multiplicative → power → unary → postfix → primary

std::unique_ptr<Expr> Parser::parse_expr() { return parse_comparison(); }

std::unique_ptr<Expr> Parser::parse_comparison() {
    std::unique_ptr<Expr> lhs = parse_concat();
    if (!lhs) return nullptr;
    while (is_comparison(peek().kind)) {
        const TokenKind op = peek().kind;
        advance();
        std::unique_ptr<Expr> rhs = parse_concat();
        if (!rhs) return nullptr;
        lhs = make_binary(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parse_concat() {
    std::unique_ptr<Expr> lhs = parse_additive();
    if (!lhs) return nullptr;
    while (peek().kind == TokenKind::Amp) {
        advance();
        std::unique_ptr<Expr> rhs = parse_additive();
        if (!rhs) return nullptr;
        lhs = make_binary(TokenKind::Amp, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parse_additive() {
    std::unique_ptr<Expr> lhs = parse_multiplicative();
    if (!lhs) return nullptr;
    while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
        const TokenKind op = peek().kind;
        advance();
        std::unique_ptr<Expr> rhs = parse_multiplicative();
        if (!rhs) return nullptr;
        lhs = make_binary(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parse_multiplicative() {
    std::unique_ptr<Expr> lhs = parse_power();
    if (!lhs) return nullptr;
    while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash) {
        const TokenKind op = peek().kind;
        advance();
        std::unique_ptr<Expr> rhs = parse_power();
        if (!rhs) return nullptr;
        lhs = make_binary(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

// ^ 右结合；底数经一元层（-2^2 = (-2)^2 = 4），指数侧允许一元符号（2^-3）。
std::unique_ptr<Expr> Parser::parse_power() {
    std::unique_ptr<Expr> base = parse_unary();
    if (!base) return nullptr;
    if (peek().kind == TokenKind::Caret) {
        advance();
        std::unique_ptr<Expr> exponent = parse_power();  // 递归下降 → 右结合
        if (!exponent) return nullptr;
        return make_binary(TokenKind::Caret, std::move(base), std::move(exponent));
    }
    return base;
}

std::unique_ptr<Expr> Parser::parse_unary() {
    const TokenKind op = peek().kind;
    if (op == TokenKind::Minus || op == TokenKind::Plus) {
        advance();
        std::unique_ptr<Expr> operand = parse_unary();  // 允许连用：--5
        if (!operand) return nullptr;
        return make_unary(op, std::move(operand));
    }
    return parse_postfix();
}

// 后缀 % 可连用：50%% = 0.005。
std::unique_ptr<Expr> Parser::parse_postfix() {
    std::unique_ptr<Expr> expr = parse_primary();
    if (!expr) return nullptr;
    while (peek().kind == TokenKind::Percent) {
        advance();
        expr = make_unary(TokenKind::Percent, std::move(expr));
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_primary() {
    const Token token = peek();  // 拷贝：advance() 会让引用失效
    switch (token.kind) {
        case TokenKind::Number:
            advance();
            return make_number(token.number);
        case TokenKind::String:
            advance();
            return make_string(token.text);
        case TokenKind::Identifier: {
            const Token ident = peek();  // 拷贝：advance() 会让引用失效
            advance();  // 先消耗标识符，便于向前看一个 Token
            if (peek().kind == TokenKind::LParen) {  // IDENT '(' → 函数调用
                advance();
                return parse_call_tail(to_upper(ident.text));
            }
            const std::string upper = to_upper(ident.text);
            if (upper == "TRUE" || upper == "FALSE") {
                return make_bool(upper == "TRUE");
            }
            std::string ref;
            if (try_cell_ref(ident.text, ref)) {
                if (peek().kind == TokenKind::Colon) {  // IDENT ':' → 范围 A1:B3
                    advance();
                    if (peek().kind != TokenKind::Identifier) {
                        fail(ErrorFactory::value());  // 缺少范围结束引用
                        return nullptr;
                    }
                    const Token end_tok = peek();
                    advance();
                    std::string end_ref;
                    if (!try_cell_ref(end_tok.text, end_ref)) {
                        fail(ErrorFactory::value());  // 范围结束不是单元格引用
                        return nullptr;
                    }
                    return make_range(ref, end_ref);
                }
                return make_ref(ref);
            }
            fail(ErrorFactory::name());  // 既非布尔也非引用 → 未知名字
            return nullptr;
        }
        case TokenKind::LParen: {
            advance();
            std::unique_ptr<Expr> inner = parse_expr();
            if (!inner) return nullptr;
            if (peek().kind != TokenKind::RParen) {
                fail(ErrorFactory::value());  // 缺少 ')'
                return nullptr;
            }
            advance();
            return inner;
        }
        default:
            fail(ErrorFactory::value());  // 意外 Token（含词法 Invalid）
            return nullptr;
    }
}

// 实参列表：'(' 已消耗；空列表或逗号分隔的完整表达式，以 ')' 结束。
// 函数名是否已知由求值层判断（此处接受任意 IDENT → #NAME? 在求值时产生）。
std::unique_ptr<Expr> Parser::parse_call_tail(std::string name) {
    auto call = make_call(std::move(name));
    if (peek().kind == TokenKind::RParen) {  // 零参调用：FN()
        advance();
        return call;
    }
    while (true) {
        std::unique_ptr<Expr> arg = parse_expr();
        if (!arg) return nullptr;
        call->args.push_back(std::move(arg));
        if (peek().kind == TokenKind::Comma) {
            advance();
            continue;
        }
        if (peek().kind == TokenKind::RParen) {
            advance();
            return call;
        }
        fail(ErrorFactory::value());  // 缺少 ',' 或 ')'
        return nullptr;
    }
}

}  // namespace fcalc
