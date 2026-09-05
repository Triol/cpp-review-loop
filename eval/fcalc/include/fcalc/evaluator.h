#pragma once
// fcalc 求值器：对解析后的公式树递归求值，支持单元格引用与 A1:B3 范围展开、
// 内置聚合函数（SUM/AVG/MIN/MAX/COUNT）、逻辑函数（IF 惰性求值、AND/OR 短路、NOT）、
// 错误传播、循环引用检测与 64 层访问深度限制。

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fcalc/error_factory.h"
#include "fcalc/parser.h"
#include "fcalc/value.h"

namespace fcalc {

// 求值环境由调用方实现（Sheet 接口）：
//   - 返回以 '=' 开头的文本 → 作为公式递归求值
//   - 返回其他非空文本      → 按字面量解释（数字 / TRUE / FALSE / 其余为字符串）
//   - 返回空串或抛出异常    → 引用解析失败 → #REF!
struct Sheet {
    virtual ~Sheet() = default;
    virtual std::string get_cell_formula(const std::string& ref) = 0;
};

// 便捷实现：内存映射表（键为规范化大写引用，如 "A1"）。
class MapSheet final : public Sheet {
public:
    std::map<std::string, std::string> cells;

    std::string get_cell_formula(const std::string& ref) override {
        const auto it = cells.find(ref);
        return it == cells.end() ? std::string() : it->second;
    }
};

// 单元格访问深度上限：进入被引用单元格算一层（顶层公式为第 0 层），
// 最多允许 64 层，第 65 层返回 #CYCLE!。
inline constexpr int kMaxCellDepth = 64;

// 求值期间的"正在求值"标记集（检测循环引用）。
using VisitedSet = std::unordered_set<std::string>;

// 求值器：解析 + 递归求值；单元格公式按公式原文缓存解析结果（共享只读 AST），
// 使 =SUM(A1:A100) 这类范围求值不再对同一单元格公式重复做词法+语法分析。
class Evaluator {
public:
    explicit Evaluator(Sheet& sheet) : sheet_(sheet) {}

    // 对以 '=' 开头的公式求值；不以 '=' 开头 → #VALUE!。
    Value evaluate(const std::string& formula);

private:
    // cell：错误细节的位置上下文 —— 顶层公式为 "(formula)"，单元格内公式为其规范化引用；
    // 求值进入某单元格后（eval_cell），上下文切换为该单元格自身的引用。
    Value eval_expr(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_unary(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_binary(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_ref(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_call(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    // 逻辑函数：IF 只求值被选中分支（惰性）；AND/OR 逐参数短路（遇可定值即停，
    // error 传播）；NOT 单参数取反。三者均在 eval_call 的急切参数收集前分发。
    Value eval_if(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_and_or(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    Value eval_not(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting);
    // 单个单元格求值（eval_ref 与范围展开共用：深度 / 循环检测 / 递归求值）。
    Value eval_cell(const std::string& ref, int depth, VisitedSet& visiting);
    // 范围展开：行主序枚举单元格并把求值结果追加到 out。
    // 范围内不存在的单元格跳过（视为空）；返回 false 时 err 为应传播的错误。
    bool expand_range(const Expr& e, const std::string& cell, int depth, VisitedSet& visiting,
                      std::vector<Value>& out, Value& err);

    Sheet& sheet_;
    // 解析缓存：键为单元格公式原文（eval_cell 每次求值都重新读取内容，Sheet 修改后
    // 键随之变化，不会命中过期条目）；值为共享只读 AST。
    std::unordered_map<std::string, std::shared_ptr<const Expr>> parse_cache_;
};

// 一步到位的自由函数。
inline Value evaluate_formula(Sheet& sheet, const std::string& formula) {
    return Evaluator(sheet).evaluate(formula);
}

}  // namespace fcalc
