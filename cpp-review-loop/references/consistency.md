# 一致性与 C/C++ 混用审查（Consistency Audit）

本章处理三类问题：**项目内部风格是否统一**、**C 习惯污染 C++ 代码**，以及**打印/落盘输出格式的统一**（L 组，多为持续迭代中渐进产生）。

方法论与其它维度相反：其它维度是"读代码找问题"，这里是**先测量、后判断**——
不拿自己的偏好当标准，拿项目自己的主导风格当标准。

## 方法：测主导，抓少数

1. **测量**：跑 `hotspots.py`，看 *Header hygiene* / *Function naming style* /
   *Member naming signals* 三节输出；或按下列各项的手动 grep 提示统计。
2. **判定**：出现次数明显多的是主导风格；少数派是偏离者，进报告。
3. **规范优先**：项目若自带规范（`.clang-format`、命名文档、AGENTS.md），以规范为准。
   主流代码偏离规范时，报"主流偏离规范"，而不是改规范去迎合主流。
4. **报法**："全项目函数以 snake_case 为主（812 处），以下 3 个文件共 37 处用
   camelCase：a.cpp、b.cpp、c.cpp"。按文件归组，不逐行罗列；偏离集中在少数新文件时按文件报告。
5. **分而治之**：超大项目允许"老模块一种风格、新模块另一种，各自内部统一"，
   但要在报告里明确指出边界在哪、以哪个为准。

严重度基准：一致性类默认 Minor；扩散到公共 API 或跨模块时升 Major；
涉及未定义行为的按 A 组升级条款处理，不按风格报。

## A. C 与 C++ 混用

C 和 C++ 是两套语言。大型 C++ 项目里 C 残留是渐进腐化的主因：每处漏网都给后人树立先例。

| 检查项 | 为什么是问题 | 信号（脚本 / 手动） |
|---|---|---|
| `void*` 当泛型用（纯 C++ 路径） | 绕过类型系统，重载/模板/虚函数全被绕开；边界强转错就是 UB | 脚本 `void* usage`；与 C ABI 互操作、线程入口 ctx 参数属合法，只报纯 C++ 内部路径 |
| type/kind 字段 + switch·if 链分发，不用虚函数 | 新增类型要改所有分支，正是多态要消灭的模式；同一分发逻辑散布多处时系统性腐化 | 手动：同一 type 字段被多处 switch，>2 处即系统性，按 Major 报 |
| 该用重载/模板处用 `fn_int` / `fn_str` 后缀命名 | 同上，类型系统没被使用 | 手动 |
| C 风格强转 `(T)x`、`(T*)x` | 无法 grep、会静默转掉 const；应使用 static_cast/reinterpret_cast 明确意图 | 脚本 `C-style cast`（只覆盖指针形态；`(int)x` 形态手动 grep） |
| `malloc` 出来的内存当对象用 | 没有执行构造函数；与 `delete`/`free` 混用是 UB | 脚本 `malloc/free family` + 人工确认对象语义 |
| `memset(this)` 或对非平凡类型 memset | 覆盖 vptr，UB | 脚本 `memset/ZeroMemory` |
| printf 族 vs 流/格式化库混用 | 两种 I/O 风格并存，项目内应统一 | 脚本 `printf family` |
| `#define` 常量 / 宏函数（.cpp 内） | 无类型、无作用域；应使用 constexpr / inline 函数 | 手动 grep `.cpp` 中的 `#define` |
| 定长 char 数组当字符串 | 溢出与 NUL 处理 bug 温床；应使用 std::string | 手动 |
| unscoped enum | 枚举值泄漏进外层作用域造成命名冲突；应使用 enum class | 脚本 `unscoped enum` |
| `NULL` / `0` 当空指针 | 应统一 nullptr | 脚本 `NULL macro` |
| `exit()` / `setjmp`·`longjmp` | 跳过析构，RAII 失效 | 脚本 `exit() call`、`setjmp/longjmp` |
| `<stdio.h>` 型 C 头 vs `<cstdio>` | 两种包含风格并存 | 手动 |

**升级条款**：凡属 UB（memset 非平凡类型、malloc 当对象、经 void* 的错误强转），
按 Blocker/Major 报告并写明 UB 依据，不归入风格类。

## B. 头文件规范

| 检查项 | 为什么 | 信号 |
|---|---|---|
| include 组内未排序 | 可 grep、可去重、减少合并冲突；防止"重排头文件"暴露隐藏依赖 | 脚本 `unsorted includes`（同括号组内字典序） |
| include 不在文件起始处 | 依赖不透明，常是隐式依赖（靠前面代码先包含）的征兆 | 脚本 `code before first include` |
| 用 `#ifdef` guard 而非 `#pragma once` | guard 宏名会撞车、会拼错；统一 `#pragma once`。例外：目标编译器确实不支持时——先确认再报 | 脚本 `#ifdef guard (no #pragma once)` |
| 头文件既无 guard 也无 `#pragma once` | 重复包含直接编译错或静默重定义 | 脚本 `header missing include guard` |
| guard 宏名与路径不对应（如 `net/addr.h` 用 `#ifndef UTIL_H`） | 两个不同文件出现同名 guard → 真实事故 | 手动 |
| `#pragma once` 与 guard 双保险 | 冗余，统一掉 | 手动 |

## C. 命名统一

先测量主导，再报少数派：

- **成员变量**：`_x` / `x_` / `m_x` / 裸名——全项目必须一种（脚本 *Member naming
  signals* 给三类计数与样本）。这不只是美观：构造器 `x(x)` 与 `this->x` 的歧义
  bug 都源于混用。
- **函数命名**：camelCase vs snake_case（脚本 *Function naming style*；注意
  ctor/dtor 天然 PascalCase，统计含噪声，判定时排除）。
- **类型命名**：struct/class/enum 的 PascalCase vs snake_case 全项目统一（手动）。
- **常量/枚举值**：`kFoo` vs `UPPER_CASE` 统一（手动）。
- **宏**：必须 UPPER_CASE（行业共识，发现即报，无需测量）。
- **namespace**：小写，尽量与目录对应（手动）。
- **文件名与类型名**：FooBar 类住在 `foo_bar.cpp` 还是 `FooBar.cpp`，统一（手动）。

## D. typedef / 类型别名

- **`typedef struct {...} X;`**：C 兼容头才需要；C++ 代码直接 `struct X {...};`
  （脚本 `typedef struct/union`）。匿名 struct + typedef 同理。
- **新代码 typedef vs using**：统一 `using X = ...`，可读性和模板别名支持更好
  （脚本 `typedef (prefer using)` 给总量；人工区分新/老代码）。
- **typedef 隐藏指针与所有权**：`typedef Foo* FooPtr` 把所有权和 const 性藏进
  别名，调用方无从判断（手动）。
- **同一类型多个别名并存**：`Packet` / `PacketT` 散布各处（手动）。

## E. 初始化

- **构造函数体内赋值，不用成员初始化列表**：非平凡成员多一次"默认构造 + 赋值"；
  const/引用成员直接编译不过（脚本 `this-> assignment` 是候选；`x_ = ...` 形态
  手动确认是否在 ctor 内）。
- **成员未初始化**：POD/指针成员没有默认值——随机值 bug 温床。这不是风格，
  按 Major/Blocker 报。
- `= NULL` / `= 0` 初始化指针 → 统一 `nullptr`。
- `{}` vs `()` 初始化统一；`{}` 自带窄化检查，倾向统一到 `{}`（若项目多数用
  `()` 则跟随项目）。
- 类内成员初始化（NSDMI）用不用统一。
- 冗余初始化：`std::string s = std::string("x");`、`int x = int(0);`（手动）。

## L. 日志与输出格式统一

打印与落盘格式是**隐性接口**：写入方与解析方往往不是同一个人，格式在持续迭代中漂移，
消费方（脚本、报表、grep）静默失效。方法论同样是先测量、后判断：

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 同类信息多种键值风格并存（`status=OK` vs `is_ok: false` vs 中文标签行） | 机器解析与 grep 依赖稳定格式；混用说明从无统一约定 | 脚本 *Log field style signals*（分隔符 =/: 分布 + 键命名风格 + 键名清单）；人工判断各输出面受众是否本就不同（人类控制台 vs 机器文件分开算） |
| 字段在迭代中改名/改位（`duration_ms`→`duration_us`；字段从自由文本字段之后挪到之前） | 旧脚本与旧解析器静默漏数据/错位；同一批日志文件多版本格式共存时统计口径分裂 | 手动：`git log -p` 搜格式串变更；同义字段名并存（如 `*_ms` 与 `*_us` 同现）按系统性报 |
| 状态/枚举取值集合漂移（中途新增一个状态值，只有部分写入点/解析点认识） | 消费方按字符串全等匹配，不认识的新值被静默丢进"其他"桶 | 手动：grep 状态字面量列出全集，核对每个写入点与解析点是否同步 |
| 自由文本字段（detail/msg/描述）后面还跟着结构化字段 | 自由文本可含空格与 `key=` 字样，其后字段永远解析不可靠——结构化字段必须全部排在自由文本之前 | 手动：核对每处拼行的字段顺序 |
| printf 族格式符与实参类型不匹配（`%d` 对 size_t、`%lu`/`%llu` 混用） | 可变参数无类型检查，错配是 UB（轻则输出错乱重则崩溃）；典型成因：迭代中改了实参类型没改格式串 | 脚本 `printf: size_t arg w/o %z`（高精度窄规则，显式 cast 不报）；编译器 `-Wformat` 可用时以其为准 |

升级条款：格式符错配属未定义行为，核实后按 Blocker/Major 报，不归入风格类。

## M. 标准库封装一致性（wrapper bypass）

项目常会对时间、随机数、CRC、日志等设施做统一封装（为了可测试、可替换、行为一致）。
封装的价值依赖"所有人都走它"：一处直接调用标准库，可测试缝（mock 时间）与行为一致性
（时区/精度/单位）就出现裂缝。方法论不变——**先找到项目自己的封装层，再量绕过散点**：

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 时间获取不统一（`time()` / chrono / `GetTickCount` 混用） | 项目有 `now_ms()` 之类封装时，绕过者让"伪造时间做测试"失效，时区/单位也可能不一致 | 脚本 *Wrapper-bypass candidates* 给各来源计数；注意墙钟（system_clock/time）与单调钟（steady_clock）用途本就不同，分用途后再判 |
| 同一设施多套实现并存（如两个 CRC 函数/查找表） | 实现各自漂移，结果不一致且难排查 | 脚本按标识符计数，≥2 个不同名实现即候选 |
| 随机数来源混用（`rand()` vs `<random>`） | `rand()` 无线程安全保证且质量差；项目若已封装随机源，散点绕过即偏离 | 脚本计数 |
| 判定顺序 | 1) grep 封装名定位统一封装 → 2) 封装存在时，直调标准库的散点按文件归组报 Minor → 3) 封装不存在但多来源并存，作为"建议收敛到单一封装"的系统性建议 | 手动 + 脚本 |
| 例外条款 | 封装层自身内部、语义确有不同（墙钟 vs 单调钟、进程级 vs 线程级）、第三方库边界适配处，均不算绕过 | 手动 |

---

公开规范吸收条目（资源所有权、类与接口、lambda、数值与异常安全，组号 F–I）
见 `standards.md`。
