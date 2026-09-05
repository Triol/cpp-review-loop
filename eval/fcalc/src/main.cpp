// fcalc 演示：构造一张小表，对若干公式求值并打印结果。
#include <iostream>
#include <string>
#include <vector>

#include "fcalc/evaluator.h"

int main() {
    using namespace fcalc;

    MapSheet sheet;
    sheet.cells = {
        {"A1", "10"},                   // 原始数字
        {"A2", "=A1*2+5%"},             // 引用 + 百分号
        {"A3", "=A1%"},
        {"B1", "hello"},                // 原始字符串
        {"B2", "=B1&\" \"&\"world\""},  // 字符串连接
        {"C1", "=1/0"},                 // 除零
        {"C2", "=C1+1"},                // 错误传播
        {"D1", "=A1>5"},                // 比较
        {"E1", "=E1"},                  // 自引用 → 循环
        {"F1", "=foo"},                 // 未知名字
        {"G1", "=-2^2"},
        {"G2", "=2^3^2"},
    };

    const std::vector<std::string> formulas = {
        "=1+2*3",
        "=2^3^2",
        "=-2^2",
        "=50%",
        "=\"a\"&1&TRUE",
        "=A2",
        "=A3",
        "=A1&\" items\"",
        "=B2",
        "=D1",
        "=\"apple\"<\"banana\"",
        "=1<\"a\"",
        "=\"a\"<TRUE",
        "=1/0",
        "=C2",
        "=E1",
        "=F1",
        "=Z99+1",
    };

    Evaluator evaluator(sheet);
    std::cout << "fcalc demo: spreadsheet formula evaluation\n";
    for (const std::string& formula : formulas) {
        const Value result = evaluator.evaluate(formula);
        std::cout << "  " << formula << "  =>  " << result.to_text() << "\n";
    }
    return 0;
}
