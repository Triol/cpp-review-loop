# 公开规范吸收清单（Standards Adoption Map）

本 skill 的审查条目来自两个层面：

1. **项目自身的一致性** —— `references/consistency.md`（A–E 组：C/C++ 混用、头文件、
   命名、typedef、初始化），方法论是"先测量主导，再抓偏离"；
2. **公开权威规范的原则级条目** —— 本文件（F–I 组，组号接续 A–E），按本 skill 的
   噪音规则裁剪后并入 checklist 的对应维度。

## 收录原则

- **只吸收原则与检查思路，不逐字搬运条文。** 各规范文本多有版权/许可限制，逐条照搬
  也会产生大量与项目无关的噪音；这里把每条压缩成"检查项 + 为什么 + 信号"。
- **项目规范 > 公开规范**（SKILL.md §2 第 3 条）。冲突时以项目现状及其自带配置为准。
- **公开规范互相冲突时以项目为准**：最典型的是异常——Google 禁用、Core Guidelines
  默认使用。跟随项目既有错误处理风格，只报项目内部的不一致（checklist §5）。
- **UB / 安全条目不降格为风格**，按 checklist 严重度基准报告。
- MISRA C++ / AUTOSAR 这类**领域规范不默认启用**：仅当项目或行业声明遵循时，把对应
  规范整体当作"项目规范"来审，并在覆盖声明里写明。

## 来源规范一览

| 来源 | 对本 skill 最有价值的部分 | 去向 |
|---|---|---|
| C++ Core Guidelines（Stroustrup / Sutter） | R 资源与所有权、C 类与继承、F 函数、I 接口、CP 并发 | F、G 组 |
| Google C++ Style Guide | 大团队工程纪律：explicit、lambda 显式捕获、related-header-first、struct/class 分工、override 纪律 | G、H 组；self-header 检查进了 hotspots.py |
| SEI CERT C++ Coding Standard | 数值与表达式安全（INT/EXP）、异常安全（ERR）、初始化次序（DCL） | I 组 |
| Meyers《Effective Modern C++》 | make_unique、override 必标、noexcept 移动构造、按值传递判据 | F、G 组 |
| Abseil Tips / Sutter GOTW | string_view/span 生命周期、"不拥有的东西不长期持有" | F 组 |
| MISRA C++ / AUTOSAR C++14 | 安全关键域红线（受限类型转换、运行期分配限制等） | 不默认吸收，见收录原则最后一条 |
| HIC++、JSF++、LLVM/Chromium style | 与上面重叠度高、多为项目自有取舍 | 不吸收 |

## F. 资源与所有权（Core Guidelines R 系列 + Meyers + Abseil）

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 裸 `new`/`delete` 表达所有权 | 所有权不可见：谁删、何时删、异常路径删不删，全靠口头约定；`unique_ptr` 把所有权写进类型 | 脚本 `raw new` + 人工确认归属；跨模块按 Major，成片按系统性模式报 |
| `new` 包进智能指针而不用 `make_unique`/`make_shared` | 异常安全（求值交错时泄漏）+ 可读性。前提：确认项目 C++14+ 且 make_* 已是主流，否则落入"现代化建议"反噪音条款 | 手动；只报与项目主流不一致者 |
| 接口里的所有权黑洞 | `Widget* create()` / `set_listener(Widget*)` 不说明谁释放，每个调用点都是泄漏或 double-free 候选 | checklist §2/§3 已列；跨模块边界一律 Major |
| `string_view` / span 被存储或逃逸 | 它们不拥有数据；存进成员、跨线程/回调传递后，源字符串随时消失 → 悬垂 | 手动 grep `string_view` 逐个核对生命周期 |
| 两阶段初始化 `Foo f; f.init();` | 半成品对象可被误用，忘调 init 即崩溃；构造即完整才是默认形态 | 手动；成片出现按系统性报 |
| `shared_ptr` 当默认所有权 | 原子引用计数开销 + 所有权图腐化；`unique_ptr` 或值语义往往够 | checklist §3（需 hot path / cycle 证据） |

## G. 类与接口（Core Guidelines C 系列 + Google）

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 单参构造缺 `explicit` | 静默隐式转换制造"看不出来"的调用点 | 手动（clang-tidy `google-explicit-constructor` 若可用）；个别报 Minor |
| 虚函数关键字违例（C.128） | 同一声明只应出现 virtual/override/final 之一。`virtual … override` 是真冗余；派生类漏标 override，则签名漂移在编译期抓不到，重载变隐藏 | 脚本 `virtual + override together`（违规）；脚本 `virtual without override` 是测量信号——区分基类声明 vs 漏标需类层次人工判断 |
| rule of five 残缺 | 写了析构/拷贝其一而其余缺省：浅拷贝、双重释放温床 | 手动；修法倾向 `= default` / `= delete` 显式化（C.80） |
| struct vs class 分工 | 有不变量的类型用 struct，把不变量藏进"看起来随便改"的形状；项目内应统一分工 | 手动测量主导（Google：纯数据聚合才用 struct） |
| 虚函数带默认实参 | 默认实参静态绑定、函数本身动态绑定 → 经基类指针与经派生类调用拿到不同实参 | 手动；发现即报 |
| class 暴露 public 数据成员 | 不变量无从维持（纯聚合 struct 除外） | 手动 |

## H. lambda 与可调用对象（Google lambda 纪律）

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 默认捕获 `[=]` / `[&]` | 捕获集不可见：`[=]` 隐式拷 `this`（悬垂 + 无谓拷贝）、`[&]` 在异步/回调里悬垂；显式列出才可审查 | 脚本 `default lambda capture`；重点核对进线程池/定时器/异步回调的 lambda |
| `[this]` 进长生命周期上下文 | 对象先死、lambda 后调 → use-after-free | 手动；与 checklist §3/§4 生命周期条联动 |
| `std::bind` | 占位符与引用语义（须 `ref()`）怪异难读；lambda 几乎全面替代 | 脚本 `std::bind (prefer lambda)` |
| 按值捕获大对象 | 显式捕获也别顺手把容器/字符串拷进去 | 手动 |

## I. 数值、表达式与异常安全（CERT INT/EXP/DCL/ERR）

| 检查项 | 为什么 | 信号 |
|---|---|---|
| 有符号/无符号混比 | `for (int i = 0; i < v.size(); ++i)`、`x == -1` 对 unsigned：关掉编译警告后就是行为 bug | 脚本 `signed/unsigned size() loop`；其余手动 |
| 隐式窄化 | 宽→窄（int→short、double→float）静默截断，事故点离源头很远 | 手动；`{}` 可拦但跟随项目现状（consistency §E） |
| 移位/溢出 UB | `1 << 31`、移位数 ≥ 位宽、有符号溢出都是 UB，优化器可任意处置 | 手动（CERT INT 系列） |
| 求值顺序依赖 | `i++ + ++i`、`f(g(), h())` 的副作用顺序；C++17 只规则化了部分 | 手动 |
| 跨 TU 静态初始化次序 | 非平凡全局对象互相引用，构造/析构次序未定义 → 崩在启动/退出 | 手动；倾向函数内 static 或 constexpr（与 checklist §1 全局状态条联动） |
| 按值 catch | 对象切片：`catch (std::exception e)` 把派生异常切成基类丢信息；应 `const&` | 脚本 `catch by value`（覆盖常见类类型形态） |

## 与脚本、checklist 的对应关系

- hotspots.py 信号：`default lambda capture`、`catch by value`、
  `signed/unsigned size() loop`、`std::bind (prefer lambda)`、
  `virtual + override together`、`virtual without override`（测量）、
  self-header 检查（.cpp 第一个 include 应为其同名头文件，Google 规则 #1）。
- checklist.md 对应维度（§2 API、§3 所有权、§4 并发、§5 错误处理）已补充引用。
- 报告时仍遵守铁律：脚本报出的是**候选**，打开上下文核实后才算发现；
  F–I 组条目同样适用于"项目现状优先"与反噪音清单。
