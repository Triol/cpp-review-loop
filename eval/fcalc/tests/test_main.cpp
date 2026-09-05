// fcalc 断言式测试运行器（无第三方库）。
// 覆盖目标：每个语法特性与每条错误路径至少一个用例。
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fcalc/evaluator.h"

using namespace fcalc;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void fail(const std::string& what) {
    ++g_failures;
    std::cout << "  [FAIL] " << g_group << " :: " << what << "\n";
}

void expect_number(const Value& v, double expected, const std::string& what) {
    ++g_checks;
    if (v.kind() != Value::Kind::Number || std::fabs(v.as_number() - expected) > 1e-9) {
        fail(what + " (expected " + format_number(expected) + ", got " + v.to_text() + ")");
    }
}

void expect_string(const Value& v, const std::string& expected, const std::string& what) {
    ++g_checks;
    if (v.kind() != Value::Kind::String || v.as_string() != expected) {
        fail(what + " (expected \"" + expected + "\", got \"" + v.to_text() + "\")");
    }
}

void expect_bool_value(const Value& v, bool expected, const std::string& what) {
    ++g_checks;
    if (v.kind() != Value::Kind::Boolean || v.as_boolean() != expected) {
        fail(what + " (expected " + (expected ? "TRUE" : "FALSE") + ", got " + v.to_text() + ")");
    }
}

void expect_error(const Value& v, ErrorType type, const std::string& what) {
    ++g_checks;
    if (!v.is_error() || v.as_error().type() != type) {
        fail(what + " (expected " + ErrorFactory::to_string(type) + ", got " + v.to_text() + ")");
    }
}

// 断言错误携带的细节文本（detail 不进入 to_text，仅供 API 查询）。
void expect_detail(const Value& v, const std::string& expected, const std::string& what) {
    ++g_checks;
    if (!v.is_error()) {
        fail(what + " (expected detail \"" + expected + "\", got non-error value " + v.to_text() +
             ")");
    } else if (v.as_error().detail() != expected) {
        fail(what + " (expected detail \"" + expected + "\", got \"" + v.as_error().detail() +
             "\")");
    }
}

void expect_text(const std::string& actual, const std::string& expected, const std::string& what) {
    ++g_checks;
    if (actual != expected) {
        fail(what + " (expected \"" + expected + "\", got \"" + actual + "\")");
    }
}

// ---- 测试辅助 Sheet ----

MapSheet make_sheet(std::initializer_list<std::pair<std::string, std::string>> items) {
    MapSheet sheet;
    for (const auto& item : items) sheet.cells[item.first] = item.second;
    return sheet;
}

// 引用链：A1..A(n) 依次 "=A下一格"，A(n+1) 为字面量 7 → 共进入单元格 n+1 次。
MapSheet chain_sheet(int links) {
    MapSheet sheet;
    for (int i = 1; i <= links; ++i) {
        sheet.cells["A" + std::to_string(i)] = "=A" + std::to_string(i + 1);
    }
    sheet.cells["A" + std::to_string(links + 1)] = "7";
    return sheet;
}

struct ThrowingSheet final : Sheet {
    std::string get_cell_formula(const std::string&) override {
        throw std::runtime_error("storage failure");
    }
};

Value ev(const std::string& formula, Sheet& sheet) { return evaluate_formula(sheet, formula); }

Value ev(const std::string& formula) {
    static MapSheet empty;
    return ev(formula, empty);
}

// ---- 迷你注册框架 ----

struct Test {
    const char* name;
    void (*fn)();
};

std::vector<Test>& tests() {
    static std::vector<Test> registry;
    return registry;
}

int register_test(const char* name, void (*fn)()) {
    tests().push_back(Test{name, fn});
    return 0;
}

}  // namespace

#define FC_TEST(NAME)                                \
    static void NAME();                              \
    [[maybe_unused]] static const int k_reg_##NAME = \
        register_test(#NAME, &NAME);                 \
    static void NAME()

// ---------- 字面量 ----------

FC_TEST(literal_number) {
    expect_number(ev("=42"), 42.0, "=42");
    expect_number(ev("=3.14"), 3.14, "=3.14");
    expect_number(ev("=.5"), 0.5, "=.5");
    expect_number(ev("=1e3"), 1000.0, "=1e3");
    expect_number(ev("=  7\t"), 7.0, "whitespace around literal");
}

// F3（审查修复回归）：数字字面量溢出（strtod ERANGE / inf）在词法层产出 Invalid token，
// 走既有语法错误路径返回 #VALUE!，不绕过全库『#VALUE!=数值溢出』约定。
FC_TEST(literal_number_overflow) {
    expect_error(ev("=1e999"), ErrorType::Value, "=1e999 literal overflow -> #VALUE!");
}

FC_TEST(literal_string) {
    expect_string(ev("=\"hi\""), "hi", "plain string");
    expect_string(ev("=\"say \"\"hi\"\"\""), "say \"hi\"", "escaped quotes");
    expect_string(ev("=\"\""), "", "empty string");
    expect_error(ev("=\"abc"), ErrorType::Value, "unterminated string");
}

FC_TEST(literal_bool) {
    expect_bool_value(ev("=TRUE"), true, "=TRUE");
    expect_bool_value(ev("=false"), false, "=false (case-insensitive)");
    expect_error(ev("=TRUEE"), ErrorType::Name, "=TRUEE is an unknown name");
}

FC_TEST(cell_reference) {
    MapSheet sheet = make_sheet({{"A1", "7"}, {"B2", "str"}, {"T1", "true"}, {"Z99", "1.5"}});
    expect_number(ev("=A1", sheet), 7.0, "=A1 -> raw number cell");
    expect_string(ev("=b2", sheet), "str", "=b2 lowercase ref -> raw string cell");
    expect_bool_value(ev("=T1", sheet), true, "=T1 -> raw bool cell");
    expect_number(ev("=Z99", sheet), 1.5, "=Z99 two-letter column");
    expect_error(ev("=A0", sheet), ErrorType::Name, "=A0 row 0 -> #NAME?");
    expect_error(ev("=A100", sheet), ErrorType::Name, "=A100 row > 99 -> #NAME?");
    expect_error(ev("=ZZZ1", sheet), ErrorType::Name, "=ZZZ1 bad column -> #NAME?");
    expect_error(ev("=A1B", sheet), ErrorType::Name, "=A1B malformed ref -> #NAME?");
}

// 引用大小写不敏感：不同大小写写法在解析层统一规范化为大写引用（a1 == A1）。
FC_TEST(reference_case_insensitive) {
    MapSheet sheet = make_sheet({{"A1", "7"}, {"B2", "3"}, {"ZZ9", "1.5"}});
    expect_number(ev("=a1", sheet), 7.0, "=a1 normalizes to A1");
    expect_number(ev("=A1", sheet), 7.0, "=A1 baseline");
    expect_number(ev("=b2", sheet), 3.0, "=b2 normalizes to B2");
    expect_number(ev("=B2", sheet), 3.0, "=B2 baseline");
    expect_number(ev("=zz9", sheet), 1.5, "=zz9 normalizes to ZZ9");
    expect_number(ev("=Zz9", sheet), 1.5, "=Zz9 normalizes to ZZ9");
    expect_number(ev("=SUM(a1:B2)", sheet), 10.0, "mixed-case range == uppercase range");
    expect_number(ev("=SUM(A1:b2)", sheet), 10.0, "mixed-case range (end lowercase)");
    // 规范化同样作用于公式单元格内部：小写引用正确解析并递归求值。
    MapSheet formulas = make_sheet({{"A1", "=b2*2"}, {"B2", "5"}});
    expect_number(ev("=A1", formulas), 10.0, "=A1 resolves lowercase ref inside formula");
    expect_number(ev("=a1", formulas), 10.0, "=a1 same cell, case-insensitive");
}

// $ 绝对引用：$A$1 / A$1 / $A1 三种形态均合法，解析时忽略 $ 语义（与 A1 相同），
// 内部规范化后 $ 被剥离（能按普通引用进入单元格求值与范围展开）。
FC_TEST(absolute_reference_dollar) {
    MapSheet sheet = make_sheet({{"A1", "7"}, {"B2", "3"}, {"C3", "5"}});
    // 四种形态指向同一单元格。
    expect_number(ev("=A1", sheet), 7.0, "=A1 relative baseline");
    expect_number(ev("=$A1", sheet), 7.0, "=$A1 column-absolute");
    expect_number(ev("=A$1", sheet), 7.0, "=A$1 row-absolute");
    expect_number(ev("=$A$1", sheet), 7.0, "=$A$1 fully absolute");
    expect_number(ev("=$a$1", sheet), 7.0, "=$a$1 lowercase absolute");
    expect_number(ev("=2*$B$2", sheet), 6.0, "absolute ref in arithmetic");
    expect_number(ev("=$B$2%", sheet), 0.03, "postfix % on absolute ref");
    // 规范化剥离 $：=$A$1 进入 A1 的公式递归求值。
    MapSheet formulas = make_sheet({{"A1", "=B2*10"}, {"B2", "4"}});
    expect_number(ev("=$A$1", formulas), 40.0, "=$A$1 evaluates A1's formula");
    // 范围端点可各自携带 '$'，四种组合等价。
    expect_number(ev("=SUM($A$1:$C$3)", sheet), 15.0, "absolute range");
    expect_number(ev("=SUM(A$1:$C3)", sheet), 15.0, "mixed-absolute range");
    expect_number(ev("=SUM($A1:C$3)", sheet), 15.0, "mixed-absolute range (marks swapped)");
    expect_number(ev("=SUM(A1:C3)", sheet), 15.0, "plain range baseline");
    // 非法 '$' 用法仍报错。
    expect_error(ev("=A$"), ErrorType::Name, "=A$ trailing '$' -> #NAME?");
    expect_error(ev("=A$B1"), ErrorType::Name, "=A$B1 '$' before letters -> #NAME?");
    expect_error(ev("=$5"), ErrorType::Value, "=$5 '$' before digit -> invalid token");
    expect_error(ev("=$"), ErrorType::Value, "=$ bare '$' -> invalid token");
}

// ---------- 运算符 ----------

FC_TEST(arithmetic) {
    expect_number(ev("=1+2"), 3.0, "=1+2");
    expect_number(ev("=5-3"), 2.0, "=5-3");
    expect_number(ev("=4*2.5"), 10.0, "=4*2.5");
    expect_number(ev("=10/4"), 2.5, "=10/4");
    expect_number(ev("=1+2*3"), 7.0, "precedence: * over +");
    expect_number(ev("=(1+2)*3"), 9.0, "parentheses override precedence");
    expect_number(ev("= 1 + 2 "), 3.0, "spaces around operators");
}

FC_TEST(power_and_unary) {
    expect_number(ev("=2^10"), 1024.0, "=2^10");
    expect_number(ev("=2^3^2"), 512.0, "^ is right-associative");
    expect_number(ev("=-5"), -5.0, "unary minus");
    expect_number(ev("=+5"), 5.0, "unary plus");
    expect_number(ev("=--5"), 5.0, "double unary minus");
    expect_number(ev("=2^-2"), 0.25, "negative exponent");
    expect_number(ev("=-2^2"), 4.0, "unary minus binds tighter than ^");
}

FC_TEST(percent) {
    expect_number(ev("=50%"), 0.5, "=50%");
    expect_number(ev("=200%%"), 0.02, "=200%% double percent");
    expect_number(ev("=1+5%"), 1.05, "postfix % binds tighter than +");
    MapSheet sheet = make_sheet({{"A1", "25"}});
    expect_number(ev("=A1%", sheet), 0.25, "=A1% percent on cell");
}

FC_TEST(concat) {
    expect_string(ev("=\"a\"&\"b\""), "ab", "string & string");
    expect_string(ev("=1&\"x\""), "1x", "number coerced to text");
    expect_string(ev("=TRUE&\"!\""), "TRUE!", "bool coerced to text");
    expect_string(ev("=2.5&\"\""), "2.5", "decimal formatting in concat");
    MapSheet sheet = make_sheet({{"A1", "3"}});
    expect_string(ev("=A1&\" items\"", sheet), "3 items", "concat with cell");
}

FC_TEST(comparison_same_type) {
    expect_bool_value(ev("=1=1"), true, "=1=1");
    expect_bool_value(ev("=1=2"), false, "=1=2");
    expect_bool_value(ev("=1<>2"), true, "=1<>2");
    expect_bool_value(ev("=2<3"), true, "=2<3");
    expect_bool_value(ev("=3<=3"), true, "=3<=3");
    expect_bool_value(ev("=4>1"), true, "=4>1");
    expect_bool_value(ev("=4>=5"), false, "=4>=5");
    expect_bool_value(ev("=\"apple\"<\"banana\""), true, "string lexicographic");
    expect_bool_value(ev("=\"a\"=\"a\""), true, "string equality");
    expect_bool_value(ev("=TRUE>FALSE"), true, "bool: true > false");
    expect_bool_value(ev("=FALSE=FALSE"), true, "bool equality");
}

// 跨类型比较（语义变更后）：可完整解析为数字的字符串按数字值比较（相等比较同规则）；
// 布尔先转数字（TRUE=1 / FALSE=0）再参与规则；不可解析字符串回退"数字 < 字符串"。
FC_TEST(comparison_cross_type) {
    // 数字 vs 可解析字符串：按数字值。
    expect_bool_value(ev("=5=\"5\""), true, "numeric string equals number (=)");
    expect_bool_value(ev("=\"5\"=5.0"), true, "numeric string equals number (reversed)");
    expect_bool_value(ev("=2<\"10\""), true, "numeric string compares by value (<)");
    expect_bool_value(ev("=\"10\"<2"), false, "numeric string compares by value (reversed <)");
    expect_bool_value(ev("=\"5\"<>5"), false, "numeric string <> number is false");
    // 数字 vs 不可解析字符串：回退旧规则"数字 < 字符串"。
    expect_bool_value(ev("=1<\"a\""), true, "unparseable string: number < string");
    expect_bool_value(ev("=\"a\"<1"), false, "unparseable string: string > number (reversed)");
    expect_bool_value(ev("=\"a\">1"), true, "unparseable string: string > number");
    expect_bool_value(ev("=1<>\"a\""), true, "unparseable string never equals number");
    // 布尔 vs 数字 / 字符串：布尔转数字后参与上述规则。
    expect_bool_value(ev("=TRUE=1"), true, "TRUE coerces to 1 (=)");
    expect_bool_value(ev("=FALSE=0"), true, "FALSE coerces to 0 (=)");
    expect_bool_value(ev("=2>TRUE"), true, "2 > TRUE(=1)");
    expect_bool_value(ev("=TRUE>999"), false, "TRUE(=1) not greater than 999");
    expect_bool_value(ev("=TRUE=\"1\""), true, "TRUE(=1) equals numeric string \"1\"");
    expect_bool_value(ev("=\"z\"<TRUE"), false, "unparseable string > TRUE(=1)");
    expect_bool_value(ev("=FALSE>\"zzz\""), false, "FALSE(=0) < unparseable string");
}

// 字符串-数字边界：只有整串（不含首尾空白）可完整解析时才按数字比较。
FC_TEST(comparison_string_number_boundary) {
    // 可解析形式：小数 / 指数 / 符号 / 前导小数点。
    expect_bool_value(ev("=\"5.0\"=5"), true, "\"5.0\" parses fully");
    expect_bool_value(ev("=\"1e3\"=1000"), true, "\"1e3\" parses via exponent");
    expect_bool_value(ev("=\"+5\"=5"), true, "\"+5\" parses with sign");
    expect_bool_value(ev("=\".5\"=0.5"), true, "\".5\" parses as 0.5");
    // "12abc"：部分数字不可完整解析 → 回退"数字 < 字符串"，且永不相等。
    expect_bool_value(ev("=12=\"12abc\""), false, "\"12abc\" not equal to 12");
    expect_bool_value(ev("=12<\"12abc\""), true, "\"12abc\" falls back: number < string");
    expect_bool_value(ev("=12>\"12abc\""), false, "\"12abc\" falls back (reversed)");
    expect_bool_value(ev("=\"12abc\"<>12"), true, "\"12abc\" <> 12");
    // 空串与含空白的串不可完整解析。
    expect_bool_value(ev("=0<\"\""), true, "empty string: number < string");
    expect_bool_value(ev("=\"\"=0"), false, "empty string not equal to 0");
    expect_bool_value(ev("=\"5 \"=5"), false, "trailing space breaks full parse");
    expect_bool_value(ev("=\" 5\"=5"), false, "leading space breaks full parse");
}

// ---------- 逻辑函数 ----------

// IF(cond, then, else)：cond 按规则转布尔，只求值被选中的分支（惰性）。
FC_TEST(logic_if) {
    expect_number(ev("=IF(TRUE,1,2)"), 1.0, "=IF(TRUE,1,2)");
    expect_number(ev("=IF(FALSE,1,2)"), 2.0, "=IF(FALSE,1,2)");
    expect_string(ev("=IF(2>1,\"yes\",\"no\")"), "yes", "comparison as condition");
    // 条件转布尔：非零数字 / TRUE / "TRUE" 为真，其余为假。
    expect_string(ev("=IF(1,\"t\",\"f\")"), "t", "nonzero number is true");
    expect_string(ev("=IF(-1,\"t\",\"f\")"), "t", "negative number is true");
    expect_string(ev("=IF(0,\"t\",\"f\")"), "f", "zero is false");
    expect_string(ev("=IF(\"TRUE\",\"t\",\"f\")"), "t", "string \"TRUE\" is true");
    expect_string(ev("=IF(\"true\",\"t\",\"f\")"), "t", "string \"true\" is true (case-insensitive)");
    expect_string(ev("=IF(\"yes\",\"t\",\"f\")"), "f", "other strings are false");
    expect_string(ev("=IF(\"1\",\"t\",\"f\")"), "f", "numeric string is not TRUE");
    expect_string(ev("=IF(\"\",\"t\",\"f\")"), "f", "empty string is false");
    // 惰性证据：未选中分支含 #DIV/0! 也不影响结果。
    expect_number(ev("=IF(TRUE,1,1/0)"), 1.0, "else branch with #DIV/0! not evaluated");
    expect_number(ev("=IF(FALSE,1/0,2)"), 2.0, "then branch with #DIV/0! not evaluated");
    expect_number(ev("=IF(TRUE,2,3)*10"), 20.0, "IF result usable in arithmetic");
    expect_number(ev("=IF(1<2,IF(FALSE,1/0,10),1/0)"), 10.0, "nested IF stays lazy");
    // 参数个数与错误传播。
    expect_error(ev("=IF(TRUE,1)"), ErrorType::Value, "IF needs 3 arguments (2 given)");
    expect_error(ev("=IF(TRUE,1,2,3)"), ErrorType::Value, "IF needs 3 arguments (4 given)");
    expect_error(ev("=IF()"), ErrorType::Value, "IF with no arguments");
    expect_error(ev("=IF(1/0,1,2)"), ErrorType::DivZero, "error condition propagates");

    MapSheet lazy = make_sheet({{"A1", "=IF(TRUE,7,B1)"}, {"A2", "=IF(FALSE,B1,7)"}, {"B1", "=B1"}});
    expect_number(ev("=A1", lazy), 7.0, "unselected cyclic branch not evaluated (then)");
    expect_number(ev("=A2", lazy), 7.0, "unselected cyclic branch not evaluated (else)");
}

// AND / OR：逐参数短路（遇可定值即停，error 不再求值），无短路时 error 传播；NOT 取反。
FC_TEST(logic_and_or_not) {
    expect_bool_value(ev("=AND(TRUE,TRUE)"), true, "AND of trues");
    expect_bool_value(ev("=AND(TRUE,FALSE)"), false, "AND with false");
    expect_bool_value(ev("=OR(FALSE,FALSE)"), false, "OR of falses");
    expect_bool_value(ev("=OR(FALSE,TRUE)"), true, "OR with true");
    expect_bool_value(ev("=AND(\"TRUE\",1)"), true, "strings and numbers coerce to bool");
    expect_bool_value(ev("=AND(1,0)"), false, "zero is false in AND");
    expect_bool_value(ev("=OR(0,2)"), true, "nonzero is true in OR");
    expect_bool_value(ev("=NOT(FALSE)"), true, "NOT false");
    expect_bool_value(ev("=NOT(3)"), false, "NOT nonzero number");
    expect_bool_value(ev("=NOT(\"TRUE\")"), false, "NOT string TRUE");
    expect_bool_value(ev("=NOT(\"no\")"), true, "NOT other string");
    // 短路：AND 遇 FALSE / OR 遇 TRUE 即停，其后参数（含 error）不再求值。
    expect_bool_value(ev("=AND(FALSE,1/0)"), false, "AND short-circuits before error");
    expect_bool_value(ev("=OR(TRUE,1/0)"), true, "OR short-circuits before error");
    expect_bool_value(ev("=AND(TRUE,FALSE,1/0)"), false, "AND short-circuits mid-list");
    expect_bool_value(ev("=OR(FALSE,TRUE,1/0)"), true, "OR short-circuits mid-list");
    // 无短路可定值时 error 传播。
    expect_error(ev("=AND(TRUE,1/0)"), ErrorType::DivZero, "AND propagates error");
    expect_error(ev("=OR(FALSE,1/0)"), ErrorType::DivZero, "OR propagates error");
    expect_error(ev("=AND(1/0,TRUE)"), ErrorType::DivZero, "error in first argument");
    expect_error(ev("=NOT(1/0)"), ErrorType::DivZero, "NOT propagates error");
    // 参数个数。
    expect_error(ev("=AND()"), ErrorType::Value, "AND with no arguments");
    expect_error(ev("=OR()"), ErrorType::Value, "OR with no arguments");
    expect_error(ev("=NOT()"), ErrorType::Value, "NOT with no arguments");
    expect_error(ev("=NOT(TRUE,FALSE)"), ErrorType::Value, "NOT takes exactly one argument");

    MapSheet sheet = make_sheet({{"A1", "1"}, {"B1", "0"}, {"A2", "0"}, {"B2", "=1/0"}});
    expect_bool_value(ev("=AND(A1:B1)", sheet), false, "AND over range: 0 makes it false");
    expect_bool_value(ev("=OR(A1:B1)", sheet), true, "OR over range: nonzero makes it true");
    expect_bool_value(ev("=AND(A2:B2)", sheet), false, "range short-circuit: 0 before error cell");
    expect_error(ev("=OR(A2:B2)", sheet), ErrorType::DivZero, "range reaches error cell");

    MapSheet cyclic = make_sheet({{"A1", "=AND(FALSE,B1)"}, {"A2", "=OR(TRUE,B1)"},
                                  {"A3", "=AND(TRUE,B1)"}, {"B1", "=B1"}});
    expect_bool_value(ev("=A1", cyclic), false, "short-circuit hides cyclic reference (AND)");
    expect_bool_value(ev("=A2", cyclic), true, "short-circuit hides cyclic reference (OR)");
    expect_error(ev("=A3", cyclic), ErrorType::Cycle, "no short-circuit hits the cycle");
}

// ---------- 错误路径 ----------

FC_TEST(error_div_zero) {
    expect_error(ev("=1/0"), ErrorType::DivZero, "=1/0");
    expect_error(ev("=0/0"), ErrorType::DivZero, "=0/0");
    expect_error(ev("=5/(2-2)"), ErrorType::DivZero, "=5/(2-2)");
    expect_error(ev("=(1/0)+1"), ErrorType::DivZero, "error propagates through +");
    expect_error(ev("=1+1/0"), ErrorType::DivZero, "error in right operand");
}

FC_TEST(error_value) {
    expect_error(ev("=\"a\"+1"), ErrorType::Value, "string in arithmetic");
    expect_error(ev("=\"a\"*\"b\""), ErrorType::Value, "string * string");
    expect_error(ev("=TRUE+1"), ErrorType::Value, "bool in arithmetic");
    expect_error(ev("=-\"x\""), ErrorType::Value, "unary minus on string");
    expect_error(ev("=\"a\"%"), ErrorType::Value, "percent on string");
    expect_error(ev("=1e308*10"), ErrorType::Value, "overflow -> #VALUE!");
    expect_error(ev("=(-1)^0.5"), ErrorType::Value, "pow domain error -> #VALUE!");
}

FC_TEST(error_parse) {
    expect_error(ev("=1+"), ErrorType::Value, "dangling binary op");
    expect_error(ev("=(1"), ErrorType::Value, "unclosed paren");
    expect_error(ev("=)"), ErrorType::Value, "unbalanced ')'");
    expect_error(ev("=1 2"), ErrorType::Value, "trailing token");
    expect_error(ev("=*3"), ErrorType::Value, "leading '*'");
    expect_error(ev("="), ErrorType::Value, "empty formula after '='");
    expect_error(ev("42"), ErrorType::Value, "missing leading '='");
    expect_error(ev(""), ErrorType::Value, "empty text");
}

// F1（审查修复回归）：解析期括号嵌套必须有深度上限 —— 超限返回 #VALUE!
// （detail 含 nesting），而不是递归下降栈溢出崩溃。深嵌套公式来自单元格内容、可被外部注入。
FC_TEST(error_parse_deep_nesting) {
    const int depth = 500;  // 远超上限（128）
    std::string formula = "=";
    formula.append(static_cast<std::size_t>(depth), '(');
    formula += '1';
    formula.append(static_cast<std::size_t>(depth), ')');
    Value v = ev(formula);
    expect_error(v, ErrorType::Value, "500-level nested parens -> #VALUE! (no crash)");
    expect_detail(v, "expression nesting too deep", "nesting detail names the limit");
}

FC_TEST(error_ref) {
    MapSheet sheet = make_sheet({{"A1", "=B9+1"}});
    expect_error(ev("=B9", sheet), ErrorType::Ref, "missing cell");
    expect_error(ev("=B9*2", sheet), ErrorType::Ref, "missing cell propagates");
    expect_error(ev("=A1", sheet), ErrorType::Ref, "missing cell inside cell propagates");
    ThrowingSheet throwing;
    expect_error(ev("=A1", throwing), ErrorType::Ref, "sheet throws -> #REF!");
}

FC_TEST(error_name) {
    expect_error(ev("=foo"), ErrorType::Name, "unknown identifier");
    expect_error(ev("=foo+1"), ErrorType::Name, "unknown identifier in expression");
}

FC_TEST(cycle_detection) {
    MapSheet self = make_sheet({{"A1", "=A1"}});
    expect_error(ev("=A1", self), ErrorType::Cycle, "self reference");

    MapSheet mutual = make_sheet({{"A1", "=B1+1"}, {"B1", "=A1*2"}});
    expect_error(ev("=A1", mutual), ErrorType::Cycle, "mutual reference");

    MapSheet loop = make_sheet({{"A1", "=B1"}, {"B1", "=C1"}, {"C1", "=A1"}});
    expect_error(ev("=A1", loop), ErrorType::Cycle, "3-cell loop");
    expect_error(ev("=C1", loop), ErrorType::Cycle, "3-cell loop entered from C1");

    MapSheet ok = chain_sheet(63);  // 64 次进入单元格：恰好在上限内
    expect_number(ev("=A1", ok), 7.0, "chain of 64 cell visits is allowed");

    MapSheet too_deep = chain_sheet(64);  // 65 次进入：超限
    expect_error(ev("=A1", too_deep), ErrorType::Cycle, "chain of 65 cell visits -> #CYCLE!");
}

// ---------- 错误细节（detail 字段） ----------

// detail 只说明原因、不进入 to_text()；随错误传播原样携带。
FC_TEST(error_detail_div_zero) {
    Value v = ev("=1/0");
    expect_error(v, ErrorType::DivZero, "=1/0 is #DIV/0!");
    expect_detail(v, "division by zero at (formula)", "=1/0 detail names location (formula)");
    expect_text(v.to_text(), "#DIV/0!", "detail is not part of to_text");

    MapSheet sheet = make_sheet({{"B2", "=1/0"}});
    Value in_cell = ev("=B2", sheet);
    expect_error(in_cell, ErrorType::DivZero, "#DIV/0! raised inside B2");
    expect_detail(in_cell, "division by zero at B2", "detail names the cell B2");

    Value propagated = ev("=2+(1/0)");
    expect_error(propagated, ErrorType::DivZero, "error propagates through +");
    expect_detail(propagated, "division by zero at (formula)", "detail survives propagation");
}

FC_TEST(error_detail_name) {
    Value v = ev("=FOO");
    expect_error(v, ErrorType::Name, "=FOO is #NAME?");
    expect_detail(v, "unknown name 'FOO'", "=FOO detail names FOO");

    Value fn = ev("=FOO(1)");
    expect_error(fn, ErrorType::Name, "=FOO(1) is #NAME?");
    expect_detail(fn, "unknown function 'FOO()' at (formula)", "unknown function detail");

    MapSheet sheet = make_sheet({{"B2", "=nope"}});
    Value in_cell = ev("=B2", sheet);
    expect_error(in_cell, ErrorType::Name, "#NAME? raised inside B2");
    expect_detail(in_cell, "unknown name 'nope'", "detail from inside cell keeps name");
}

FC_TEST(error_detail_cycle) {
    MapSheet self = make_sheet({{"A1", "=A1"}});
    Value v = ev("=A1", self);
    expect_error(v, ErrorType::Cycle, "self reference is #CYCLE!");
    expect_detail(v, "circular reference involving 'A1'", "cycle detail names A1");

    MapSheet deep = chain_sheet(64);  // 第 65 次进入 A65 时超限
    Value d = ev("=A1", deep);
    expect_error(d, ErrorType::Cycle, "chain of 65 cell visits -> #CYCLE!");
    expect_detail(d, "cell depth limit (64) exceeded at 'A65'", "depth detail names A65");
}

FC_TEST(error_detail_ref) {
    MapSheet sheet = make_sheet({{"A1", "=B9+1"}});
    Value v = ev("=B9", sheet);
    expect_error(v, ErrorType::Ref, "=B9 missing cell");
    expect_detail(v, "missing cell 'B9'", "missing-cell detail names B9");

    Value inner = ev("=A1", sheet);
    expect_error(inner, ErrorType::Ref, "missing cell propagates through A1");
    expect_detail(inner, "missing cell 'B9'", "propagated ref detail preserved");

    ThrowingSheet throwing;
    Value t = ev("=A1", throwing);
    expect_error(t, ErrorType::Ref, "sheet throws -> #REF!");
    expect_detail(t, "cell lookup failed for 'A1' (storage exception)", "storage exception detail");
}

FC_TEST(cell_evaluation_semantics) {
    MapSheet sheet = make_sheet({{"A1", "2"}, {"A2", "=A1*3"}, {"A3", "=A2+A1"}});
    expect_number(ev("=A2", sheet), 6.0, "referenced formula evaluated recursively");
    expect_number(ev("=A3", sheet), 8.0, "multi-level references");

    MapSheet broken = make_sheet({{"A1", "=1+"}, {"B1", "=A1+1"}});
    expect_error(ev("=B1", broken), ErrorType::Value, "parse error inside cell propagates");

    MapSheet named = make_sheet({{"A1", "=nope"}});
    expect_error(ev("=A1", named), ErrorType::Name, "#NAME? inside cell propagates");

    MapSheet cyc = make_sheet({{"A1", "=B1"}, {"B1", "=B1"}});
    expect_error(ev("=A1", cyc), ErrorType::Cycle, "cycle inside cell propagates");
}

FC_TEST(error_dominates_operations) {
    MapSheet sheet = make_sheet({{"A1", "=1/0"}});
    expect_error(ev("=1/0=1/0"), ErrorType::DivZero, "error compared with error");
    expect_error(ev("=\"a\"&(1/0)"), ErrorType::DivZero, "error in concat");
    expect_error(ev("=A1&A1", sheet), ErrorType::DivZero, "cell errors in concat");
}

// ---------- 范围语法 ----------

FC_TEST(range_in_functions) {
    MapSheet sheet = make_sheet({{"A1", "1"}, {"B1", "2"}, {"A2", "3"}, {"B2", "4"}});
    expect_number(ev("=SUM(A1:B2)", sheet), 10.0, "=SUM(A1:B2) 2x2 range");
    expect_number(ev("=SUM(B2:A1)", sheet), 10.0, "reversed endpoints normalized");
    expect_number(ev("=SUM(a1:b2)", sheet), 10.0, "lowercase range normalized");
    expect_number(ev("=SUM(A1:A1)", sheet), 1.0, "single-cell range");
    expect_number(ev("=COUNT(A1:B2)", sheet), 4.0, "COUNT over range");
    expect_number(ev("=MIN(A1:B2)", sheet), 1.0, "MIN over range");
    expect_number(ev("=MAX(A1:B2)", sheet), 4.0, "MAX over range");
    expect_number(ev("=AVG(A1:B2)", sheet), 2.5, "AVG over range");
    expect_number(ev("=SUM((A1:B2))", sheet), 10.0, "parenthesized range still expands");
    expect_number(ev("=SUM(AVG(A1:A2),1)", sheet), 3.0, "nested call taking a range");

    // 行主序：按 A1,B1,A2,B2 顺序遇到第一个 error（B1 先于 A2）
    MapSheet order = make_sheet(
        {{"A1", "1"}, {"B1", "=1/0"}, {"A2", "=\"a\"+1"}, {"B2", "2"}});
    expect_error(ev("=SUM(A1:B2)", order), ErrorType::DivZero,
                 "row-major: B1 error found before A2");

    // 双字母列 + 端点颠倒
    MapSheet wide = make_sheet({{"Y1", "10"}, {"Z1", "20"}});
    expect_number(ev("=SUM(Y1:Z1)", wide), 30.0, "two-letter column range");
    expect_number(ev("=SUM(Z1:Y1)", wide), 30.0, "two-letter column range reversed");

    // 混合参数：值 / 引用 / 范围
    expect_number(ev("=SUM(100,A1,A1:B2)", sheet), 111.0, "mixed literal + ref + range");
}

FC_TEST(range_empty_and_missing) {
    MapSheet partial = make_sheet({{"A1", "5"}, {"B3", "str"}});
    expect_number(ev("=SUM(A1:B3)", partial), 5.0, "missing cells in range are skipped");
    expect_number(ev("=COUNT(A1:B3)", partial), 1.0, "COUNT skips missing cells");
    expect_number(ev("=AVG(A1:B3)", partial), 5.0, "AVG over single remaining number");

    MapSheet empty;
    expect_number(ev("=SUM(A1:B3)", empty), 0.0, "SUM over empty range = 0");
    expect_number(ev("=COUNT(A1:B3)", empty), 0.0, "COUNT over empty range = 0");
    expect_error(ev("=AVG(A1:B3)", empty), ErrorType::Value, "AVG over empty range -> #VALUE!");
    expect_error(ev("=MIN(A1:B3)", empty), ErrorType::Value, "MIN over empty range -> #VALUE!");
    expect_error(ev("=MAX(A1:B3)", empty), ErrorType::Value, "MAX over empty range -> #VALUE!");

    MapSheet texts = make_sheet({{"A1", "x"}, {"A2", "y"}});
    expect_number(ev("=SUM(A1:A2)", texts), 0.0, "SUM over string-only range = 0");
    expect_error(ev("=AVG(A1:A2)", texts), ErrorType::Value, "no aggregatable values -> #VALUE!");
}

FC_TEST(range_parse_and_bare) {
    MapSheet sheet = make_sheet({{"A1", "1"}, {"B2", "2"}});
    expect_error(ev("=A1:B2", sheet), ErrorType::Value, "bare range outside function");
    expect_error(ev("=A1:B2+1", sheet), ErrorType::Value, "range in arithmetic");
    expect_error(ev("=-A1:B2", sheet), ErrorType::Value, "unary on range");
    expect_error(ev("=A1:", sheet), ErrorType::Value, "missing range end");
    expect_error(ev("=A1:B", sheet), ErrorType::Value, "range end without row");
    expect_error(ev("=A1:5", sheet), ErrorType::Value, "range end not an identifier");
    expect_error(ev("=A1:B2:C3", sheet), ErrorType::Value, "trailing ':' chain");
}

// ---------- 内置函数 ----------

FC_TEST(function_sum) {
    expect_number(ev("=SUM(1,2,3)"), 6.0, "=SUM(1,2,3)");
    expect_number(ev("=SUM(1.5,2.5)"), 4.0, "decimal arguments");
    expect_number(ev("=SUM(10)"), 10.0, "single argument");
    expect_number(ev("=SUM()"), 0.0, "no arguments -> 0");
    expect_number(ev("=SUM(-1,3)"), 2.0, "negative argument");
    expect_number(ev("=SUM(1+2,3*4)"), 15.0, "expression arguments");
    expect_number(ev("=SUM(\"a\",TRUE,7)"), 7.0, "strings and bools ignored");
    expect_number(ev("=SUM(\"5\")"), 0.0, "numeric string is not coerced");
    expect_number(ev("=sum(1,2)"), 3.0, "lowercase function name");
    expect_number(ev("=Sum(1,2)"), 3.0, "mixed-case function name");

    MapSheet sheet = make_sheet({{"A1", "2"}, {"A2", "=A1*10"}, {"B1", "x"}});
    expect_number(ev("=SUM(A1,A2,1)", sheet), 23.0, "cell refs (with formulas) as arguments");
    expect_number(ev("=SUM(A1:B1,5)", sheet), 7.0, "range skips string cell");
}

FC_TEST(function_avg) {
    expect_number(ev("=AVG(2,4,6)"), 4.0, "=AVG(2,4,6)");
    expect_number(ev("=AVG(5)"), 5.0, "single value");
    expect_number(ev("=avg(2,4)"), 3.0, "lowercase AVG");
    expect_number(ev("=AVG(1,2,\"a\",TRUE)"), 1.5, "non-numbers ignored");
    expect_error(ev("=AVG(\"a\",TRUE)"), ErrorType::Value, "no numbers -> #VALUE!");
    expect_error(ev("=AVG()"), ErrorType::Value, "no arguments -> #VALUE!");
}

FC_TEST(function_min_max) {
    expect_number(ev("=MIN(3,1,2)"), 1.0, "=MIN(3,1,2)");
    expect_number(ev("=MAX(3,1,2)"), 3.0, "=MAX(3,1,2)");
    expect_number(ev("=MIN(-5,2)"), -5.0, "negative values");
    expect_number(ev("=MAX(2,10,5)"), 10.0, "MAX picks largest");
    expect_number(ev("=MIN(\"a\",TRUE,3,7)"), 3.0, "non-numbers ignored by MIN");
    expect_number(ev("=MAX(\"a\",TRUE,3,7)"), 7.0, "non-numbers ignored by MAX");
    expect_number(ev("=min(4,9)"), 4.0, "case-insensitive MIN");
    expect_error(ev("=MIN(TRUE)"), ErrorType::Value, "MIN with no numbers -> #VALUE!");
    expect_error(ev("=MAX()"), ErrorType::Value, "MAX with no arguments -> #VALUE!");
}

FC_TEST(function_count) {
    expect_number(ev("=COUNT(1,\"a\",TRUE,2,3)"), 3.0, "counts only numbers");
    expect_number(ev("=COUNT()"), 0.0, "no arguments -> 0");
    expect_number(ev("=COUNT(\"1\",TRUE)"), 0.0, "strings and bools not counted");
    expect_number(ev("=count(5)"), 1.0, "case-insensitive COUNT");

    MapSheet sheet = make_sheet({{"A1", "1"}, {"B1", "x"}, {"A2", "=A1+1"}, {"B2", "TRUE"}});
    expect_number(ev("=COUNT(A1:B2)", sheet), 2.0, "range: formulas count, text/bool do not");
    expect_number(ev("=COUNT(A1:B2,99,\"y\")", sheet), 3.0, "mixed list with range");
}

FC_TEST(function_error_paths) {
    expect_error(ev("=FOO(1)"), ErrorType::Name, "unknown function -> #NAME?");
    expect_error(ev("=foo(1)"), ErrorType::Name, "unknown function lowercase -> #NAME?");
    expect_error(ev("=SUM(1,1/0)"), ErrorType::DivZero, "error argument propagates");
    expect_error(ev("=COUNT(1/0)"), ErrorType::DivZero, "error propagates through COUNT too");

    MapSheet cells = make_sheet({{"A1", "=1/0"}, {"B1", "=nope"}, {"C1", "=SUM(A1:A1)"}});
    expect_error(ev("=SUM(A1:A1)", cells), ErrorType::DivZero, "error inside ranged cell");
    expect_error(ev("=SUM(B1:B1)", cells), ErrorType::Name, "#NAME? inside ranged cell");
    expect_error(ev("=C1", cells), ErrorType::DivZero, "SUM inside cell propagates via ref");

    MapSheet cyclic = make_sheet({{"A1", "=SUM(A1:A2)"}, {"A2", "1"}});
    expect_error(ev("=A1", cyclic), ErrorType::Cycle, "self reference through range -> #CYCLE!");

    ThrowingSheet throwing;
    expect_error(ev("=SUM(A1:A3)", throwing), ErrorType::Ref, "sheet throws during range -> #REF!");

    expect_error(ev("=SUM(1,2"), ErrorType::Value, "missing ')'");
    expect_error(ev("=SUM(1,)"), ErrorType::Value, "trailing comma");
    expect_error(ev("=SUM(,1)"), ErrorType::Value, "leading comma");
    expect_error(ev("=SUM(1 2)"), ErrorType::Value, "missing comma between args");
}

// ---------- 工厂与文本化 ----------

FC_TEST(factory_and_text) {
    // 唯一用于校验工厂输出拼写的字面量（规范本体在 error_factory.h）。
    expect_text(Value::error(ErrorFactory::div_zero()).to_text(), "#DIV/0!", "DivZero text");
    expect_text(Value::error(ErrorFactory::value()).to_text(), "#VALUE!", "Value text");
    expect_text(Value::error(ErrorFactory::ref()).to_text(), "#REF!", "Ref text");
    expect_text(Value::error(ErrorFactory::name()).to_text(), "#NAME?", "Name text");
    expect_text(Value::error(ErrorFactory::cycle()).to_text(), "#CYCLE!", "Cycle text");
    expect_text(Value::number(1024.0).to_text(), "1024", "integer formatting");
    expect_text(Value::number(-3.5).to_text(), "-3.5", "negative decimal formatting");
    expect_text(Value::boolean(true).to_text(), "TRUE", "bool to text");
    // detail 重载：携带时 round-trip 可查，缺省为空；detail 不进入 to_text。
    expect_detail(Value::error(ErrorFactory::div_zero("division by zero at B2")),
                  "division by zero at B2", "factory detail round-trip");
    expect_detail(Value::error(ErrorFactory::value()), "", "factory default detail is empty");
    expect_text(Value::error(ErrorFactory::name("unknown name FOO()")).to_text(), "#NAME?",
                "detail excluded from to_text");
    expect_detail(Value::error(Error(ErrorType::Value, "constructor detail")), "constructor detail",
                  "Error constructor accepts detail");
}

int main() {
    std::size_t passed = 0;
    for (const Test& test : tests()) {
        g_group = test.name;
        std::cout << "[ RUN  ] " << test.name << "\n";
        const int failures_before = g_failures;
        try {
            test.fn();
        } catch (const std::exception& e) {
            fail(std::string("unexpected exception: ") + e.what());
        } catch (...) {
            fail("unexpected exception (non-standard)");
        }
        if (g_failures == failures_before) {
            ++passed;
            std::cout << "[ OK   ] " << test.name << "\n";
        }
    }
    std::cout << "----------------------------------------\n"
              << passed << "/" << tests().size() << " test groups passed, "
              << g_checks << " checks, " << g_failures << " failures\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "TESTS FAILED\n";
    return 1;
}
