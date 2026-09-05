#include "fcalc/evaluator.h"

#include "text_util.h"

#include <cmath>
#include <cstdlib>

namespace fcalc {
namespace {

// 整串可完整解析为数字时成功；不接受首尾空白（" 5" / "5 " 均失败），"12abc" / "" / "+"
// 均失败。比较规则用它判断"数字 vs 字符串"能否按数字比较；interpret_literal 调用前已
// 自行 trim 原始单元格文本。仅当首字符是数字 / 正负号 / 小数点时才尝试 strtod，
// 避免 "inf" / "nan" 被 strtod 静默接受。
bool try_parse_number(const std::string& t, double& out) {
    if (t.empty()) return false;
    const char c = t[0];
    if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) return false;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;
    out = v;
    return true;
}

// 比较（语义变更后的新规则）：
//   数字 vs 数字：按值；字符串 vs 字符串：字典序；布尔 vs 布尔：FALSE < TRUE；
//   数字 vs 字符串：字符串可完整解析为数字则按数字比较，否则按"数字 < 字符串"；
//   布尔 vs 数字 / 字符串：布尔转数字（TRUE=1, FALSE=0）后参与上述规则。
// 相等比较同规则。返回 -1 / 0 / 1。
int compare_values(const Value& l, const Value& r) {
    const auto num_cmp = [](double a, double b) {
        if (a < b) return -1;
        return (b < a) ? 1 : 0;
    };
    // 布尔 vs 布尔：仍按 FALSE < TRUE，不转数字。
    if (l.kind() == Value::Kind::Boolean && r.kind() == Value::Kind::Boolean) {
        if (l.as_boolean() == r.as_boolean()) return 0;
        return l.as_boolean() ? 1 : -1;
    }
    // 布尔 vs 数字 / 字符串：转数字后走下方规则。
    if (l.kind() == Value::Kind::Boolean) {
        return compare_values(Value::number(l.as_boolean() ? 1.0 : 0.0), r);
    }
    if (r.kind() == Value::Kind::Boolean) {
        return compare_values(l, Value::number(r.as_boolean() ? 1.0 : 0.0));
    }
    // 数字 vs 字符串：可完整解析则按数字，否则按"数字 < 字符串"。
    if (l.kind() == Value::Kind::Number && r.kind() == Value::Kind::String) {
        double d = 0.0;
        if (try_parse_number(r.as_string(), d)) return num_cmp(l.as_number(), d);
        return -1;
    }
    if (l.kind() == Value::Kind::String && r.kind() == Value::Kind::Number) {
        double d = 0.0;
        if (try_parse_number(l.as_string(), d)) return num_cmp(d, r.as_number());
        return 1;
    }
    // 剩余为同类型：数字按值 / 字符串按字典序。
    switch (l.kind()) {
        case Value::Kind::Number:
            return num_cmp(l.as_number(), r.as_number());
        case Value::Kind::String: {
            const int c = l.as_string().compare(r.as_string());
            return (c < 0) ? -1 : (c > 0 ? 1 : 0);
        }
        default:
            return 0;  // 不可达：错误在比较前已被拦截
    }
}

bool is_comparison(TokenKind op) {
    switch (op) {
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

bool apply_comparison(TokenKind op, int c) {
    switch (op) {
        case TokenKind::Eq: return c == 0;
        case TokenKind::NotEq: return c != 0;
        case TokenKind::Less: return c < 0;
        case TokenKind::LessEq: return c <= 0;
        case TokenKind::Greater: return c > 0;
        default: return c >= 0;  // GreaterEq
    }
}

// 非公式单元格内容 → 字面量：数字 / TRUE / FALSE / 其余按字符串。
Value interpret_literal(const std::string& raw) {
    const std::string s = trim(raw);
    const std::string upper = to_upper(s);
    if (upper == "TRUE") return Value::boolean(true);
    if (upper == "FALSE") return Value::boolean(false);
    double v = 0.0;
    if (try_parse_number(s, v)) return Value::number(v);
    return Value::string(s);
}

// IF 条件与 AND / OR / NOT 参数共用的"值 → 布尔"规则：
// 非零数字 true；布尔原样；字符串恰为 "TRUE"（忽略大小写与首尾空白）true；其余 false。
bool coerce_to_bool(const Value& v) {
    switch (v.kind()) {
        case Value::Kind::Number: return v.as_number() != 0.0;
        case Value::Kind::Boolean: return v.as_boolean();
        case Value::Kind::String: return to_upper(trim(v.as_string())) == "TRUE";
        default: return false;  // 不可达：调用方已拦截 error
    }
}

// 规范化引用（如 "A1"）→ 列号（A=1，Z=26，AA=27，ZZ=702）与行号。
// 解析层已保证格式，这里仅做防御性校验。
bool parse_ref_coords(const std::string& ref, int& col, int& row) {
    std::size_t i = 0;
    int c = 0;
    while (i < ref.size() && ref[i] >= 'A' && ref[i] <= 'Z') {
        c = c * 26 + (ref[i] - 'A' + 1);
        ++i;
    }
    if (c == 0 || i >= ref.size()) return false;
    int r = 0;
    for (; i < ref.size(); ++i) {
        if (ref[i] < '0' || ref[i] > '9') return false;
        r = r * 10 + (ref[i] - '0');
    }
    if (r < 1) return false;
    col = c;
    row = r;
    return true;
}

// 列号 → 字母（与 parse_ref_coords 互逆）：1 → "A"，26 → "Z"，27 → "AA"。
std::string column_to_letters(int col) {
    std::string s;
    while (col > 0) {
        const int rem = (col - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + rem));
        col = (col - 1) / 26;
    }
    return s;
}

// 错误细节的位置后缀：cell 为 "(formula)"（顶层公式）或单元格引用（如 "B2"）。
std::string at(const std::string& cell) { return " at " + cell; }

// 运算符 → 展示文本（用于错误细节）。
const char* op_text(TokenKind op) {
    switch (op) {
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Caret: return "^";
        case TokenKind::Percent: return "%";
        case TokenKind::Amp: return "&";
        case TokenKind::Eq: return "=";
        case TokenKind::NotEq: return "<>";
        case TokenKind::Less: return "<";
        case TokenKind::LessEq: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEq: return ">=";
        default: return "?";
    }
}

// 范围表达式 → 归一化行列坐标（端点颠倒时交换，如 B2:A1 → A1:B2）。
// 解析层已保证引用格式，失败属不可达（防御性返回 false）。
bool range_bounds(const Expr& e, int& c1, int& r1, int& c2, int& r2) {
    if (!parse_ref_coords(e.text, c1, r1) || !parse_ref_coords(e.text_end, c2, r2)) return false;
    if (c1 > c2) std::swap(c1, c2);
    if (r1 > r2) std::swap(r1, r2);
    return true;
}

// 内置聚合函数（SUM / AVG / MIN / MAX / COUNT；IF / AND / OR / NOT 在 eval_call
// 前走专用路径，不会到达这里。实参中的 error 已在收集阶段传播）。
// 约定：只对 number 聚合，string / boolean 忽略；无可聚合数值时 SUM = 0，
// AVG / MIN / MAX → #VALUE!；聚合结果溢出 → #VALUE!（同算术溢出约定）。
Value apply_function(const std::string& name, const std::vector<Value>& args,
                     const std::string& cell) {
    if (name == "COUNT") {
        int n = 0;
        for (const Value& v : args) {
            if (v.kind() == Value::Kind::Number) ++n;
        }
        return Value::number(static_cast<double>(n));
    }
    if (name == "SUM" || name == "AVG" || name == "MIN" || name == "MAX") {
        double sum = 0.0;
        double mn = 0.0;
        double mx = 0.0;
        int count = 0;
        for (const Value& v : args) {
            if (v.kind() != Value::Kind::Number) continue;
            const double x = v.as_number();
            if (count == 0) {
                mn = x;
                mx = x;
            } else {
                if (x < mn) mn = x;
                if (x > mx) mx = x;
            }
            sum += x;
            ++count;
        }
        if (count == 0) {
            return name == "SUM"
                       ? Value::number(0.0)
                       : Value::error(ErrorFactory::value(
                             "'" + name + "' has no numeric values to aggregate" + at(cell)));
        }
        double out = 0.0;
        if (name == "SUM") {
            out = sum;
        } else if (name == "AVG") {
            out = sum / count;
        } else if (name == "MIN") {
            out = mn;
        } else {
            out = mx;
        }
        if (!std::isfinite(out)) {
            return Value::error(ErrorFactory::value(
                "'" + name + "' result is not finite (overflow or domain error)" + at(cell)));
        }
        return Value::number(out);
    }
    return Value::error(ErrorFactory::name("unknown function '" + name + "()'" + at(cell)));
}

}  // namespace

Value Evaluator::evaluate(const std::string& formula) {
    const ParseResult parsed = parse_formula(formula);
    if (!parsed.ok) return Value::error(parsed.error);
    VisitedSet visiting;
    return eval_expr(*parsed.expr, "(formula)", 0, visiting);
}

Value Evaluator::eval_expr(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting) {
    switch (e.kind) {
        case Expr::Kind::Number: return Value::number(e.number);
        case Expr::Kind::String: return Value::string(e.text);
        case Expr::Kind::Bool: return Value::boolean(e.boolean);
        case Expr::Kind::Ref: return eval_ref(e, cell, depth, visiting);
        case Expr::Kind::Range:
            // 裸范围不是标量值：范围只能作为函数参数（在那里按行主序展开）。
            return Value::error(
                ErrorFactory::value("range outside a function argument" + at(cell)));
        case Expr::Kind::Call: return eval_call(e, cell, depth, visiting);
        case Expr::Kind::Unary: return eval_unary(e, cell, depth, visiting);
        case Expr::Kind::Binary: return eval_binary(e, cell, depth, visiting);
    }
    return Value::error(
        ErrorFactory::value("internal error: unhandled expression kind" + at(cell)));  // 不可达
}

Value Evaluator::eval_unary(const Expr& e, const std::string& cell, int depth,
                            VisitedSet& visiting) {
    Value v = eval_expr(*e.lhs, cell, depth, visiting);
    if (v.is_error()) return v;  // 错误传播
    switch (e.op) {
        case TokenKind::Plus:
            return v;  // 一元 + 原样返回
        case TokenKind::Minus:
            if (v.kind() != Value::Kind::Number) {
                return Value::error(
                    ErrorFactory::value("unary '-' requires a numeric operand" + at(cell)));
            }
            return Value::number(-v.as_number());
        case TokenKind::Percent:
            if (v.kind() != Value::Kind::Number) {
                return Value::error(
                    ErrorFactory::value("unary '%' requires a numeric operand" + at(cell)));
            }
            return Value::number(v.as_number() / 100.0);
        default:
            return Value::error(ErrorFactory::value("unknown unary operator" + at(cell)));
    }
}

Value Evaluator::eval_binary(const Expr& e, const std::string& cell, int depth,
                             VisitedSet& visiting) {
    Value l = eval_expr(*e.lhs, cell, depth, visiting);
    Value r = eval_expr(*e.rhs, cell, depth, visiting);
    if (l.is_error()) return l;  // 错误传播：任一操作数为 error 则结果即该 error
    if (r.is_error()) return r;

    if (e.op == TokenKind::Amp) {
        // 字符串连接：非字符串操作数先转文本
        return Value::string(l.to_text() + r.to_text());
    }
    if (is_comparison(e.op)) {
        return Value::boolean(apply_comparison(e.op, compare_values(l, r)));
    }

    // 剩余为算术运算：仅接受数字
    if (l.kind() != Value::Kind::Number || r.kind() != Value::Kind::Number) {
        return Value::error(ErrorFactory::value(std::string("operator '") + op_text(e.op) +
                                                "' requires numeric operands" + at(cell)));
    }
    const double a = l.as_number();
    const double b = r.as_number();
    double out = 0.0;
    switch (e.op) {
        case TokenKind::Plus: out = a + b; break;
        case TokenKind::Minus: out = a - b; break;
        case TokenKind::Star: out = a * b; break;
        case TokenKind::Slash:
            if (b == 0.0) {
                return Value::error(ErrorFactory::div_zero("division by zero" + at(cell)));
            }
            out = a / b;
            break;
        case TokenKind::Caret: out = std::pow(a, b); break;
        default:
            return Value::error(ErrorFactory::value(std::string("unknown binary operator '") +
                                                    op_text(e.op) + "'" + at(cell)));
    }
    if (!std::isfinite(out)) {  // 溢出 / 定义域外
        return Value::error(ErrorFactory::value(std::string("operator '") + op_text(e.op) +
                                                "' overflowed or hit a domain error" + at(cell)));
    }
    return Value::number(out);
}

Value Evaluator::eval_ref(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting) {
    (void)cell;  // 引用错误的位置信息由被引用单元格自身（eval_cell）提供
    return eval_cell(e.text, depth, visiting);
}

// 单个单元格求值：深度限制 + 循环引用检测 + 递归求值（引用与范围展开共用）。
// 进入单元格后，错误细节的位置上下文切换为该单元格自身的引用。
Value Evaluator::eval_cell(const std::string& ref, int depth, VisitedSet& visiting) {
    const int next = depth + 1;  // 进入被引用单元格加深一层
    if (next > kMaxCellDepth) {
        return Value::error(ErrorFactory::cycle("cell depth limit (" +
                                                std::to_string(kMaxCellDepth) +
                                                ") exceeded at '" + ref + "'"));
    }

    // 标记法：正在求值的单元格再次被遇到 → 循环引用
    if (!visiting.insert(ref).second) {
        return Value::error(ErrorFactory::cycle("circular reference involving '" + ref + "'"));
    }
    struct Guard {
        VisitedSet& set;
        const std::string& ref;
        ~Guard() { set.erase(ref); }
    } guard{visiting, ref};

    std::string content;
    try {
        content = sheet_.get_cell_formula(ref);
    } catch (...) {
        return Value::error(ErrorFactory::ref("cell lookup failed for '" + ref +
                                              "' (storage exception)"));  // 解析失败（异常）
    }
    if (content.empty()) {
        return Value::error(ErrorFactory::ref("missing cell '" + ref + "'"));  // 解析失败（不存在）
    }

    if (content[0] == '=') {  // 单元格内是公式 → 递归求值
        // 解析缓存：以公式原文为键复用共享只读 AST，避免范围求值（如 =SUM(A1:A100)）
        // 对同一单元格公式重复做完整词法+语法分析。内容每次重新读取，缓存键即内容本身，
        // Sheet 修改后自然反映新状态；解析失败的公式不缓存（每次重试，保持既有行为）。
        std::shared_ptr<const Expr> expr;
        const auto it = parse_cache_.find(content);
        if (it != parse_cache_.end()) {
            expr = it->second;
        } else {
            const ParseResult parsed = parse_formula(content);
            if (!parsed.ok) return Value::error(parsed.error);
            expr = parsed.expr;
            parse_cache_.emplace(content, expr);
        }
        return eval_expr(*expr, ref, next, visiting);
    }
    return interpret_literal(content);  // 单元格内是原始字面量
}

// 范围展开：把 start..end 归一化后按行主序（先行后列）枚举每个单元格。
// 范围内不存在的单元格跳过（视为空，与聚合时忽略 string / bool 一致）；
// 存在但求值为 error 的单元格传播该错误。
bool Evaluator::expand_range(const Expr& e, const std::string& cell, int depth,
                             VisitedSet& visiting, std::vector<Value>& out, Value& err) {
    int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
    if (!range_bounds(e, c1, r1, c2, r2)) {
        err = Value::error(ErrorFactory::value("invalid range bounds" + at(cell)));  // 不可达
        return false;
    }
    for (int row = r1; row <= r2; ++row) {
        for (int col = c1; col <= c2; ++col) {
            const std::string ref = column_to_letters(col) + std::to_string(row);
            std::string content;
            try {
                content = sheet_.get_cell_formula(ref);
            } catch (...) {
                err = Value::error(ErrorFactory::ref("cell lookup failed for '" + ref +
                                                     "' (storage exception)"));  // 存储层异常
                return false;
            }
            if (content.empty()) continue;  // 空 / 缺失单元格：不计入
            out.push_back(eval_cell(ref, depth, visiting));
            if (out.back().is_error()) {
                err = out.back();
                return false;
            }
        }
    }
    return true;
}

// IF(cond, then, else)：cond 按规则转布尔后只求值被选中的分支（惰性），
// 未选中分支完全不求值（其中的 #DIV/0! / 循环引用都不会触发）。
// 参数个数必须恰为 3，否则 #VALUE!；cond 求值为 error 时传播。
Value Evaluator::eval_if(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting) {
    if (e.args.size() != 3) {
        return Value::error(ErrorFactory::value("IF requires exactly 3 arguments, got " +
                                                std::to_string(e.args.size()) + at(cell)));
    }
    Value cond = eval_expr(*e.args[0], cell, depth, visiting);
    if (cond.is_error()) return cond;
    return eval_expr(*e.args[coerce_to_bool(cond) ? 1 : 2], cell, depth, visiting);
}

// AND / OR：参数逐个求值，错误立即传播；AND 遇 FALSE / OR 遇 TRUE 即可定结果
// 并停止（短路），其后参数不再求值。范围实参按行主序逐单元格惰性求值：
// 每求值一个单元格即做一次短路判定，可定值之后的单元格（含其中的 error）不接触。
// 全部求完仍不可定：AND → TRUE，OR → FALSE；无参数 → #VALUE!。
Value Evaluator::eval_and_or(const Expr& e, const std::string& cell, int depth,
                             VisitedSet& visiting) {
    const bool is_and = (e.text == "AND");
    if (e.args.empty()) {
        return Value::error(ErrorFactory::value(
            "'" + std::string(is_and ? "AND" : "OR") + "' requires at least 1 argument" + at(cell)));
    }
    // 短路判定：该值是否已能定下 AND / OR 的结果（AND 遇 FALSE / OR 遇 TRUE）。
    const auto decides = [is_and](bool b) { return is_and ? !b : b; };
    for (const auto& arg : e.args) {
        if (arg->kind == Expr::Kind::Range) {
            int c1 = 0, r1 = 0, c2 = 0, r2 = 0;
            if (!range_bounds(*arg, c1, r1, c2, r2)) {
                return Value::error(
                    ErrorFactory::value("invalid range bounds" + at(cell)));  // 不可达
            }
            for (int row = r1; row <= r2; ++row) {
                for (int col = c1; col <= c2; ++col) {
                    const std::string ref = column_to_letters(col) + std::to_string(row);
                    std::string content;
                    try {
                        content = sheet_.get_cell_formula(ref);
                    } catch (...) {
                        return Value::error(ErrorFactory::ref(
                            "cell lookup failed for '" + ref + "' (storage exception)"));
                    }
                    if (content.empty()) continue;  // 空 / 缺失单元格：不计入
                    Value v = eval_cell(ref, depth, visiting);
                    if (v.is_error()) return v;  // 错误先于任何可定值被遇到 → 传播
                    if (decides(coerce_to_bool(v))) return Value::boolean(!is_and);
                }
            }
            continue;  // 该范围未定下结果 → 继续下一实参
        }
        Value v = eval_expr(*arg, cell, depth, visiting);
        if (v.is_error()) return v;
        if (decides(coerce_to_bool(v))) return Value::boolean(!is_and);
    }
    return Value::boolean(is_and);
}

// NOT(x)：单参数转布尔后取反；参数个数非 1 → #VALUE!，error 传播。
Value Evaluator::eval_not(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting) {
    if (e.args.size() != 1) {
        return Value::error(ErrorFactory::value("NOT requires exactly 1 argument, got " +
                                                std::to_string(e.args.size()) + at(cell)));
    }
    Value v = eval_expr(*e.args[0], cell, depth, visiting);
    if (v.is_error()) return v;
    return Value::boolean(!coerce_to_bool(v));
}

// 函数调用：IF / AND / OR / NOT 走惰性 / 短路专用路径（在急切收集前分发）；
// 其余函数逐参数收集值（范围参数展开为多值），任一 error 立即传播，
// 然后按函数名（已规范化为大写）分发。
Value Evaluator::eval_call(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting) {
    if (e.text == "IF") return eval_if(e, cell, depth, visiting);
    if (e.text == "AND" || e.text == "OR") return eval_and_or(e, cell, depth, visiting);
    if (e.text == "NOT") return eval_not(e, cell, depth, visiting);

    std::vector<Value> values;
    for (const auto& arg : e.args) {
        if (arg->kind == Expr::Kind::Range) {
            Value err = Value::error(
                ErrorFactory::value("range expansion failed" + at(cell)));  // 占位，展开时覆盖
            if (!expand_range(*arg, cell, depth, visiting, values, err)) return err;
            continue;
        }
        Value v = eval_expr(*arg, cell, depth, visiting);
        if (v.is_error()) return v;
        values.push_back(std::move(v));
    }
    return apply_function(e.text, values, cell);
}

}  // namespace fcalc
