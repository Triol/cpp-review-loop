#pragma once
// fcalc 错误工厂：全库唯一的 "#XXX" 错误文本来源。
// 团队规范：任何 error 值必须经 ErrorFactory 创建；禁止在其他文件手写 "#XXX" 字符串字面量。

#include <string>
#include <utility>

namespace fcalc {

// 电子表格风格错误类型。
enum class ErrorType {
    DivZero,  // #DIV/0!  除零
    Value,    // #VALUE!  非法操作数 / 语法错误 / 数值溢出
    Ref,      // #REF!    引用解析失败
    Name,     // #NAME?   未知标识符
    Cycle,    // #CYCLE!  循环引用 / 超过访问深度上限
};

// 不可变错误值：类型 + 可选细节（说明具体原因，如 "division by zero at B2"）。
// 展示文本统一由 ErrorFactory 生成且只含 "#XXX"；detail 不进入 to_text()，
// 仅供 API 查询（detail()），错误传播时随 Error 对象原样携带。
class Error {
public:
    explicit Error(ErrorType type = ErrorType::Value, std::string detail = std::string())
        : type_(type), detail_(std::move(detail)) {}

    ErrorType type() const { return type_; }
    const std::string& detail() const { return detail_; }
    std::string text() const;  // 定义在 ErrorFactory 之后（依赖其 to_string）

private:
    ErrorType type_;
    std::string detail_;
};

// 中央错误工厂 —— error 值的唯一合法构造入口。
// 每类创建函数接受可选 detail（默认空串 = 无细节）；调用方应传入有信息量的说明。
class ErrorFactory {
public:
    static Error div_zero(std::string detail = std::string()) {
        return Error(ErrorType::DivZero, std::move(detail));
    }
    static Error value(std::string detail = std::string()) {
        return Error(ErrorType::Value, std::move(detail));
    }
    static Error ref(std::string detail = std::string()) {
        return Error(ErrorType::Ref, std::move(detail));
    }
    static Error name(std::string detail = std::string()) {
        return Error(ErrorType::Name, std::move(detail));
    }
    static Error cycle(std::string detail = std::string()) {
        return Error(ErrorType::Cycle, std::move(detail));
    }

    // 错误类型 → 展示文本。本 switch 是 "#XXX" 字面量在全库的唯一出现点。
    static const char* to_string(ErrorType type) {
        switch (type) {
            case ErrorType::DivZero: return "#DIV/0!";
            case ErrorType::Value: return "#VALUE!";
            case ErrorType::Ref: return "#REF!";
            case ErrorType::Name: return "#NAME?";
            case ErrorType::Cycle: return "#CYCLE!";
        }
        return "#VALUE!";
    }
};

inline std::string Error::text() const { return ErrorFactory::to_string(type_); }

}  // namespace fcalc
