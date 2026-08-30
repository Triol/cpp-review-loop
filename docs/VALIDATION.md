# 验证用例与实测记录（Validation Cases）

本文档记录 cpp-review-loop skill 的验证方法、全部测试用例与实测结果。所有用例于
2026-08-30 执行（Windows 11、Python 3.14、双盲子 agent 设计），供使用者与后续
改进时参考。技能本体的使用说明见 [README](../README.md)。

## 1. 端到端有效性测试（双盲）

### 1.1 设计

回答一个问题：**一个没见过这个 skill 的 agent，拿着它能做出多好的审查？**

- **实现者（子 agent A）**：只拿到一份不提"审查 / bug / 质量"字眼的产品需求
  （进程内任务调度器：优先级队列 + N 工作线程 + 懒取消 + 原子指标 + key=value 配置，
  其中持久化模块按需求硬性要求为"从十年前 C 项目移植的 legacy 风格"），
  产出 10 个文件、954 行代码。需求条款锁死交付物形态，隔离性不依赖 agent 自觉。
- **审查者（子 agent B）**：全新上下文，只拿到 skill 路径
  （`~/.agents/skills/cpp-review-loop/SKILL.md`），要求按 SKILL.md 完整流程执行并
  产出全部交付物。
- **评分基准**：由主 agent 独立通读全部 954 行代码事先建立，再与 B 的报告对照。
  实验原始产物（被审项目、findings、报告）公开在 [`eval/task-scheduler/`](../eval/task-scheduler/)。

> 注：本节 E2 的审查者按"单个冷启动 agent 全量执行"方式运行。skill 此后按用户要求把
> 默认执行模型改为**编排式 subagent 派发**（主 agent 建图与汇总；深读按"维度 × 文件批"
> 派发聚焦 subagent，每批返回过集中检查点——抽查核实、去重、合并后才派下一批；宿主无
> subagent 能力时退化为顺序自执行，见 SKILL.md §2 执行模型）。修复轮同样改为编排式
> TDD-LOOP（行为类修复测试先行：建复现单测→红→改代码→绿，测试留作回归覆盖；
> 实现 subagent 写代码、主 agent 逐批审查、收尾由不知情的新 subagent 复审全部被改
> 文件，SKILL.md §7）。两种模式共享同一套清单与证据规则，本文档的查准/
> 噪音结论对两者同样适用；细粒度派发与复审闭环的分批效果未单独量化。

### 1.2 Case E1 · 实现阶段不触发 skill（隔离性）

| 验证点 | 结果 |
|---|---|
| 实现者是否加载/调用 skill | ❌ 未触发（其任务词面不含任何触发词；description 明确排除 "writing unrelated new C++ features"） |
| 交付物是否被"审查意识"净化 | 否——legacy C 风格模块为需求硬性条款，后续审查正是针对它验证噪音抑制 |

### 1.3 Case E2 · 冷启动审查的查准率（8/8）

B 产出 8 条发现（1 Major + 5 Minor + 2 Nit），逐条回源码核实：

| # | 发现 | 判定 | 备注 |
|---|---|---|---|
| F1 | Major · `result_writer_close` 与并发 append 的生命周期契约缺口（UAF 机制分析） | ✅ 成立 | skill 规则内 Major 可辩护（"边界所有权模糊=Major"）；`delete` 在解锁后的机制分析正确；修法给的是文档补契约 + RAII 包装而非瞎改 |
| F2 | Minor · `validate()` 漏 worker_threads 上限 256；构造函数不校验 config | ✅ 成立 | 真实的不对称校验缺口 |
| F3 | Minor · 并发调用 `shutdown()` 时第二个调用者提前返回，违背"阻塞直到排空"契约 | ✅ 成立 | **评分基准遗漏、审查者多抓到的真并发问题**（对比人工快读的优势实证） |
| F4 | Minor · 懒取消任务仍占队列容量，可把队列占满导致新任务被拒 | ✅ 成立 | 文档化设计 + 真实失效模式 |
| F5 | Minor · `persistence_enabled=TURE` 被静默当作 false（无告警） | ✅ 成立 | 同类配置项均有告警唯独它没有 |
| F6 | Minor · 零测试覆盖（核心纯逻辑易测却无测试） | ✅ 成立 | 维度内合理 |
| F7 | Nit · 公共头包含 `<string>` 但自身未使用 | ✅ 成立 | IWYU 类真 nit |
| F8 | Nit · 演示代码收集 `ids` 后从未使用 | ✅ 成立 | 死代码 |

### 1.4 Case E3 · 噪音抑制（本次测试最关键的指标）

hotspots.py 在该仓库扫出 **8 类 60+ 条红旗候选**（printf 族 ×26、NULL 宏 ×17、
mutex 类型 ×11、`catch(...)` ×2、memset ×2、sleep ×1、typedef struct ×1、typedef ×1，
另有 `#ifdef guard` 头文件卫生候选与 `_mkdir` 被误认成下划线开头成员的启发式误报）。

**进入最终报告的误报：0 条。**

- legacy 模块的 NULL / typedef struct / FILE* / snprintf 等 C 风格写法全部按
  "项目规范优先 + 候选≠发现"规则正确过滤（模块注释明示"按团队约定保持 C 风格"）；
- 两处 `catch(...)`（构造函数回滚重抛、任务异常隔离后转 FAILED）均为正确用法，未报；
- `_mkdir` 的成员命名误报未被采信；
- 调度器并发逻辑（锁外执行、谓词等待、比较器、懒取消）经逐行核对后**没有被虚构出**假竞态。

### 1.5 Case E4 · 查全对比

- **审查者超出基准**：F3（并发 shutdown 语义缺陷）为评分基准遗漏。
- **基准中有而审查者遗漏**：
  - `e.what()` 含空格会破坏持久化模块声明的日志行格式契约（Minor，跨模块格式问题——
    此遗漏直接催生了 consistency.md §L，见 §3）；
  - `localtime` 失败未检查 → 1970 时间戳（Nit）；
  - `cmake_minimum_required(VERSION 3.10)` 过老（Nit）；
  - `pendingCount()` 把懒取消任务计入排队数（Nit，F4 已覆盖同一根因）。
- **"正确的不报"同样计为通过**：命名 snake_case（legacy）/camelCase（新代码）、
  `#pragma once` 与 `#ifdef` guard 并存——均沿"分而治之/约定优先"条款处理，未逐行罗列。

### 1.6 Case E5 · 流程符合度

| 流程要求 | 结果 |
|---|---|
| 先跑 hotspots.py 建图再深读 | ✅ 摘要中引用了测量结果 |
| 候选必须开上下文核实 | ✅ 明确声明"未带未经核实的 grep 命中进报告" |
| 覆盖度声明（强制） | ✅ 写明覆盖 8/8 文件；未编译运行；无 git 历史；clang-tidy 存在但工具链缺失→按 tooling.md 规则不搭环境、跳过并声明 |
| 系统性模式单独聚合 | ✅ 1 条模式（3 处）独立成节 |
| 聊天报告保持摘要级 | ✅ 明细只在 HTML |
| 不修改被审查代码 | ✅ 以文件 mtime 核实（16:50–16:53 均为实现者写入，报告 17:04 生成） |
| 交付物齐全合规 | ✅ findings JSON 符合 report-format.md schema；HTML 渲染正常（headless 截图核验）；报告头部 repo 路径正确 |

### 1.7 结论

**查准 8/8，误报 0/60+ 候选，流程全符合，且比人工基准多抓 1 个真问题**；
代价是漏掉 1 个 Minor 级跨模块契约问题与 3 个 Nit。

---

## 2. hotspots.py 信号验证（正反例夹具）

用专门构造的 C++ 夹具逐个验证新增信号（2026-08-30，`py_compile` + 实跑）：

| 信号 | 应命中（实测 ✅） | 应排除（实测 ✅ 未报） |
|---|---|---|
| default lambda capture | `[=]`、`[&]` | `[&v]`、`[x = 1]` |
| catch by value | `catch(std::exception e)`、`catch(MyErr ex)` | `catch(const std::exception& e)`、`catch(int code)` |
| signed/unsigned size() loop | `for (int i = 0; i < v.size(); ++i)` | — |
| std::bind (prefer lambda) | `std::bind(...)` | — |
| virtual + override together | `virtual void draw() const override;` | — |
| virtual without override（测量信号） | `virtual void tick();` | `= 0` 纯虚、`~dtor`、`final` |
| self-header not first include | `widget.cpp` 首个 include 是 `<string>` 而非自身头 | 无同名头的 .cpp 不适用 |

**回归 case**：新 self-header 信号在 demo 项目（examples/demo-netstack）上立刻抓到
2 处真违规——此前修复轮给 `pool.cpp` 加 `<mutex>`、`row.cpp` 加 `<memory>` 时把系统头
放在了自身头文件之前（正是 Google 规则 #1 不允许的）；改为自身头在前后信号清零
（"headers look uniform"）。同时验证了 `virtual without override` 在 `row.h`
基类声明上**只测量不裁决**的设计语义。

**追加批次（构建加固信号，2026-08-30）**：hotspots.py 新增 *Build hardening
signals*——扫描 CMake / MSVC / Make / Bazel / Meson 构建文件，按 6 类测量旗标在场
情况：warnings 等级、warnings-as-errors、strict 转换（-Wconversion/-Wshadow/
/permissive-）、hardening 运行时（stack-protector、_FORTIFY_SOURCE、
_GLIBCXX_ASSERTIONS、/sdl、/guard:cf）、sanitizer、§7 全局旗标反模式
（add_compile_options / set(CMAKE_CXX_FLAGS)）：

| Case | 结果 |
|---|---|
| 正例夹具（-Wall / -Wextra / -Werror / -Werror=specific / -fstack-protector-strong / -fsanitize= / 全局反模式） | 全部识别；`-Werror (all)` 与 `-Werror=specific` 正确区分为两个信号 |
| CMake 裸定义形式 | `target_compile_definitions(... _FORTIFY_SOURCE=2 _GLIBCXX_ASSERTIONS)`（无 `-D` 前缀）修正后可识别 |
| MSVC 旗标 | `/W4`、`/WX`、`/permissive-` 均识别 |
| task-scheduler 实测 | 识别 `-Wall -Wextra -Wpedantic`、`/W4`、`/permissive-`；werror / hardening / sanitizers 三类如实输出"未检测到"并附建议文案；`add_compile_options` ×2 反模式命中 |
| demo-netstack 实测 | 构建文件存在但零旗标 → "no warning/hardening flags detected at all" + 四类建议 |
| 语义约束 | "未检测到"只是建议（按工具链判断、vendored 目录可豁免），输出与文档均标注 candidates |
| 运行时代价告知（用户要求） | 检测到有运行时代价的旗标时只对用户告警、不提出"删除修复"，去留由用户决定；扫描输出按类别内联标注代价：sanitizers（ASan ≈2x CPU / 2-3x 内存，仅限 dev/CI，绝不上 release）、hardening（~0-3%，通常可忽略）、warnings/-Werror 类（纯编译期零运行时代价，无需警告）。夹具实测各类别标注均正确输出 |
| 头文件拆分信号（用户要求追加） | 新增 *Header composition*（每头文件的列首 class/struct 定义计数）：夹具 5 类全识别、前置声明 `class Beta;` 正确排除、模板类与换行大括号形态均识别；task-scheduler 实测 3 个双类头（如 metrics.h 的 MetricsSnapshot+Metrics）——全部是"主类+配套辅助结构"的合法形态，恰好演示"候选不裁决、相关性人工判断"的设计 |
| 封装绕过一致性（§M，用户要求追加） | 新增 *Wrapper-bypass candidates*：按设施测量时间/随机数来源多样性、CRC 同名实现数。task-scheduler 实测：时间获取 3 种来源并存（time()/std::time、system_clock、steady_clock），并附"墙钟 vs 单调钟用途不同、分用途后再判"提示——本项目墙钟记日志、单调钟测耗时属合法分工，恰好演示"候选不裁决"；rand/crc 单一来源未误报。修复轮同步加入"diff 不得引入不必要依赖（新 #include / 链接库 / 第三方测试框架）"检查点条款 |
| 第三方时间源覆盖（用户追问） | brpc 的 `butil::gettimeofday_us()`、SPDK 的 `spdk_get_ticks()`、`clock_gettime`、`QueryPerformanceCounter` 等第三方/OS 时间源扩充进 *Wrapper-bypass candidates*。夹具实测：gettimeofday*(2) + spdk_get_ticks(2) + time()/std::time(1) 识别为 3 种来源并存；eval 项目回归结果不变。`WRAPPER_TIME_RES` 为纯数据表，项目特有的封装面一行即可追加 |

checklist.md §7 的 "Warnings" 条目同步扩为 "Warnings & warnings-as-errors" 与
"Robustness compile options absent" 两条。

---

## 3. 迭代漂移实验（催生 consistency.md §L 的实验）

### 3.1 设计

回答用户的两个假设：**日志格式统一性** 与 **printf 格式符混用（%ld/%u/%lu）是否会在
持续迭代中自然出现**。方法：3 个互不知情的子 agent 依次"接手项目"提需求迭代
（每轮都是全新上下文 + 只给功能需求 + 不提格式/日志/bug 任何字眼）：

| 轮次 | 需求 | 与格式相关的产出 |
|---|---|---|
| 起点 | （双盲实验项目，954 行） | 文件日志单一写入方单一格式；printf 全部显式 cast 正确 |
| 迭代 1 | 任务重试 + 任务标签 + 按优先级计数 | 日志行新增 `tag=` 字段与新状态 `RETRYING` |
| 迭代 2 | 读日志文件做统计报表 + `--json` + 周期任务 | **解析方（stats_report）与写入方分离**；日志行新增 `type=` |
| 迭代 3 | 运维要求：记录 worker 编号、耗时改微秒 | 字段 `duration_ms`→`duration_us` 改名、字段顺序重排 |

起点基线核实：**两类问题在当时都不存在**（26 处 printf 族逐一核对格式符/实参全匹配；
日志格式单一）。

### 3.2 迭代后的漂移证据（日志格式统一性：验证成立）

1. **字段顺序三代演变**：v0 `ts task_id status duration_ms detail` → v1/v2 把
   `tag`/`type` 追加在自由文本 `detail` **之后** → v3 整体重排为
   `ts task_id status tag type worker duration_us detail`。新旧格式并存时，
   解析方只能按"detail 之前"的位置约定取字段，**旧格式行的 tag 被静默归入 "-" 桶**。
2. **字段改名**：`duration_ms`（毫秒小数）→ `duration_us`（整数微秒），头文件需要
   大段注释解释兼容——任何 `grep duration_ms=` 的运维脚本从此静默漏数据。
3. **四层 ABI 考古层**：`append → append_ex → append_ex2 → append_ex3`，每轮迭代
   叠一层；不同调用点用不同版本（`cancel()` 用 ex2 → CANCELLED 行 worker/duration
   恒为 "-"，消费方必须知道这条隐含规则）。
4. **状态集合膨胀**：`OK/FAILED/CANCELLED` 中途加入 `RETRYING`，写入方与解析方
   靠人肉保持同步，漏一处即静默漏计。

### 3.3 printf 格式符混用：未复现（如实记录）

三轮后所有 printf 族调用仍保持显式 cast + 严格匹配。原因分析：

- 初版作者的显式 cast 纪律成为"既有风格"，后续 agent 通读全码后局部修改，天然模仿；
- **实验偏差**：每轮迭代者都是全上下文的"仔细开发者"；真实世界的格式符漂移多来自
  只改一行不看全局的部分上下文编辑，本实验的设计天然抑制了这类错误；
- 编译器交叉验证不可用（本机 clang 缺 MSVC STL 头，`-Wformat` 无法运行——与审查
  agent 的覆盖声明互相印证）。

结论：未复现 ≠ 不值得查（UB 类问题、`-Wformat` 的存在即证明其普遍性），
按"高精度窄规则"纳入扫描器。

### 3.4 由本实验新增的 skill 内容

| 内容 | 位置 |
|---|---|
| L 组「日志与输出格式统一」：键值风格并存、字段改名/改位、状态集合漂移、自由文本字段后跟结构化字段、printf 格式符错配（核实后按 UB 报 Blocker/Major） | references/consistency.md §L |
| printf 格式符/实参错配条目（优先引用 `-Wformat`） | references/checklist.md §5 |
| 日志格式统一指针 | references/checklist.md §2 |
| 信号：*Log field style signals*、`printf: size_t arg w/o %z` | scripts/hotspots.py |

### 3.5 追加迭代（第 4/5 轮）：函数膨胀与结构性重复

用户追加验证两个编码行为：**① 函数体过长、不切分子逻辑**；**② 公用逻辑不抽象、各自实现**。
方法同前：两个互不知情的子 agent 依次接手（轮 4 需求 = CSV 导出 + `--out` 写文件 + p95；
轮 5 需求 = `--trace` 追踪埋点 + 死信队列 + `trace_enabled` 配置），前后用同一把尺子测量
（hotspots 最长函数排名 + 连续 5 行归一化窗口的精确重复块检测）。

| 测量点 | 3 轮后（基线） | 5 轮后 | 结论 |
|---|---|---|---|
| 最大函数 | `main` 159 行 | `main` **218 行** | ✅ 持续膨胀，全程无人切分 |
| 次大函数 | `printDailyTagStats` 64 / `runOne` 62 | `runOne` **86**（trace+死信+重试+持久化混排）、`dailyTagStatsToText` 82 | ✅ |
| >50 行函数个数 | 3 个 | 5 个（main / runOne / ToText / ToJson 53 / ToCsv 50） | ✅ |
| 精确重复块（5 行窗口） | 2 处（~6 行，tag 表 vs worker 表打印块） | **0 处**（迭代 4 重写渲染后窗口失效） | 机械精确行检测太脆 |
| 结构性重复 | 字段清单 2 处 | **tag 字段清单需在 4 处手工同步**（parseLine / ToText / ToJson / ToCsv）；trace 事件格式在 8 个调用点各自拼写 | ✅ 真正的风险形态 |

**判定：两个行为都会在编码迭代中自然出现，需要加入审查**，但手段不同：

- 函数长度：hotspots.py 已有测量（longest functions），缺的是判断指导——行数本身不是
  发现，**混合抽象层次 / 函数内分节注释 / 含糊命名**才是；新增 checklist §1 条目
  "God functions"。
- 重复：机械逐行去重被证明太脆（重命名一个字段即失效），不做机械信号；新增 checklist
  §1 条目 "Copy-paste-adapted logic without a shared abstraction"（信号 = 同一字段清单
  在写入方与解析方各列举一次、同构序列化块、同 enum→string 映射写两遍；修法 = 单一
  字段清单/参数化渲染）。

---

## 4. 新信号实测（更新后的 hotspots.py）

### 4.1 正反例夹具

| 信号 | 命中（实测 ✅） | 排除（实测 ✅ 未报） |
|---|---|---|
| printf: size_t arg w/o %z | `printf("%lu items", v.size())` | `printf("%zu items", v.size())`；`printf("%d", static_cast<int>(v.size()))`；snprintf 的 `sizeof(buf)` 目的缓冲不触发 |
| Log field style signals | `"status=OK tag=x"` → 2 个 key=value 键 | URL 里的 `a:b` 不计；格式串 `%d:` 借助 `%` 前瞻排除不计 |

### 4.2 真实项目（三轮迭代后的 task-scheduler）实测

```text
-- Log/output field style signals (CANDIDATES; judge by audience) --
   16  key=value       0  key:value
    7  key style snake_case
    5  key style lowercase
    4  key style PascalCase
        distinct keys: worker_threads(1), queue_capacity(1), ..., task_id(1), status(1), tag(1)
        watch: same field two names (*_ms vs *_us), mixed separators
printf-signal: 0
```

键风格三分（snake_case / lowercase / PascalCase 混用）与 16 个键值对一目了然；
printf 检查器在该项目诚实报 0（代码确实无错配）——"测量不造谣"的行为符合设计。

---

## 5. 已知局限

- 单一测试项目、每轮迭代者均为全上下文修改，抑制了"部分上下文编辑"类错误的自然出现
  （printf 格式符混用未复现的主因，见 §3.3）；
- 查全对比基于主 agent 的独立人工基准，非穷尽式清单；
- 本机编译器无法解析标准库头，printf 错配未能与 `-Wformat` 交叉验证；
- 反馈闭环（HTML 逐条表态 → 导出 feedback JSON → 修复轮）的 UI 部分在此前的
  人工验收中验证过（含自定义意见、comment-only 项导出为 discuss、编号点击复制、
  localStorage 刷新不丢），本文档未重复覆盖。
