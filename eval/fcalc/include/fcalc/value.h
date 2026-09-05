#pragma once
// fcalc 值模型：number / string / bool / error 四种。

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "fcalc/error_factory.h"

namespace fcalc {

// 数字 → 文本（& 连接与结果显示共用）：整数不带小数点，其余保留 15 位有效数字。
inline std::string format_number(double v) {
    char buf[40];
    if (v == 0.0) return "0";  // -0.0 == 0.0 为真：负零归一化，避免 "%.0f" 输出 "-0"
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.15g", v);
    }
    return buf;
}

// 四种值：Number / String / Boolean / Error。
class Value {
public:
    enum class Kind { Number, String, Boolean, Error };

    static Value number(double v) {
        Value x;
        x.kind_ = Kind::Number;
        x.number_ = v;
        return x;
    }
    static Value string(std::string s) {
        Value x;
        x.kind_ = Kind::String;
        x.string_ = std::move(s);
        return x;
    }
    static Value boolean(bool b) {
        Value x;
        x.kind_ = Kind::Boolean;
        x.boolean_ = b;
        return x;
    }
    static Value error(Error e) {
        Value x;
        x.kind_ = Kind::Error;
        x.error_ = std::move(e);
        return x;
    }

    Kind kind() const { return kind_; }
    bool is_error() const { return kind_ == Kind::Error; }

    // 以下访问器仅在对应 kind 下有效。
    double as_number() const { return number_; }
    const std::string& as_string() const { return string_; }
    bool as_boolean() const { return boolean_; }
    const Error& as_error() const { return error_; }

    // 文本化（错误值 → 其 "#XXX" 文本）。
    std::string to_text() const {
        switch (kind_) {
            case Kind::Number: return format_number(number_);
            case Kind::String: return string_;
            case Kind::Boolean: return boolean_ ? "TRUE" : "FALSE";
            case Kind::Error: return error_.text();
        }
        return "";
    }

private:
    Value() = default;

    Kind kind_ = Kind::Number;
    double number_ = 0.0;
    std::string string_;
    bool boolean_ = false;
    Error error_{ErrorType::Value};
};

}  // namespace fcalc
