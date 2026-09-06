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
| 魔数检查 + 每批修复后复扫（用户要求追加） | 新增 *Magic-number candidates*：统计常量定义行（#define/const/constexpr/enum）之外的两位数以上字面量（字符串/注释已剥离、十六进制与小数排除）。夹具实测：42 / 86400 / 512 计入 3 个候选，具名常量定义与枚举值正确排除；eval 项目实测 30 个候选/7 文件（缓冲区大小类留待人工判断）。SKILL.md §7 同步升级：修复前快照 hotspots 基线 → 每批检查点复扫被改文件对比基线（新引入的魔数/裸 new/新时间源当场归入本批解决）→ 收尾复审前全项目复扫对比 |
| 弱模型防遗漏加固（用户要求） | SKILL.md 新增 §0 执行门（审查轮 G1-G8 / 修复轮 F1-F6，每道门有必须实际存在的产出物，缺失即未通过）+ 报告前逐项自检清单；make_report.py 升级为硬校验——缺 `review.coverage`/`summary`/`scope` 或任一 finding 缺 `file/title/detail/fix` 时拒绝渲染并输出中文报错清单。实测：合规的 demo findings 渲染通过（PASS）；人工去掉 coverage 与 fix 的文件被拒绝并精确列出 2 处缺失 |

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

---

## 6. 自我进化轮（第二轮端到端验证，编排式执行模型）

第二轮验证在 skill 完成 §0 执行门、编排式派发、TDD-LOOP、§L/§M 等全部增强后进行，
**目的：验证新执行模型与新增检查在真实演化项目上的有效性，并让暴露的问题反哺 skill**。

### 6.1 设计

- **开发**：两个互不知情的子 agent 按真实产品需求实现并迭代 `eval/logpipe/`（多源日志
  采集管道：文件 tail + stdin、解析/过滤/限流、滚动输出 + CRC32、util 封装层
  [时间/诊断日志/CRC——团队约定"时间读取必须走 util"作为 §M 的判定锚点]）。
  初始 1290 行，两轮迭代后 1597 行；需求不含任何 bug/格式/审查暗示。
- **审查**：主 agent 担任编排者，按 §2 执行模型跑 G1-G8 门——hotspots 建图后按
  **维度×文件批并行派发 4 个聚焦 subagent**（所有权/错误处理、并发、一致性 §L/§M、
  头文件/魔数/构建），每批返回过集中检查点（抽查核实→去重→合并）。
- **修复**：主 agent 以用户身份给出混合表态（accept 6 / custom 1 / discuss 1 /
  reject 1 / 未表态 10，考验 decided<total 提醒路径），按 F1-F6 门跑 TDD-LOOP
  修复（无测试设施→经批准搭建最小零依赖 assert harness，tests/ 目录）。
- **收尾**：全项目复扫对比基线 + 不知情 fresh-eyes subagent 复审被改文件。

### 6.2 结果

| 指标 | 结果 |
|---|---|
| 原始发现 → 聚合后 | 28 条 → 19 条（1 Major + 15 Minor + 3 Nit；魔数散布聚为系统性模式 S1） |
| 查准 | 每批抽查全部成立；B1 的 Major（poll_file 不清 failbit → 文件追加内容永远读不到，tail 核心功能失效）经代码复读 + 子 agent 最小复现实验双重确认 |
| 超出编排者基准 | failbit Major、stoull 负值回绕绕过校验、close() flush 静默丢数据、异常屏障缺失（terminate+丢 offset 检查点）、pending 无上限 OOM——均为编排者通读时遗漏 |
| 查全缺口 | ① 性能维度未派发（编排者计划疏漏，LogParser 每行 std::regex 未评估；两个 subagent 的 out_of_scope 均有提示）；② tailer「丢行仍推进 offset」、localtime 返回值未检两个候选无人报 |
| 新信号实测 | §M：正确发现 tailer 直调 steady_clock 违反 util 约定（同时正确提示墙钟/单调钟分用途）；头文件组成：pipeline.h 7 类判为"相关共置"不报、只报 config.h 拖入实现；魔数：30 候选/7 文件；构建加固：缺失项+运行时代价+用户决定措辞全部正确 |
| 门禁合规 | G1-G8 全部产出物到位（findings 经 make_report 硬校验渲染成功）；修复轮红绿证据真实（F1 的红：CHECK_STR_EQ failed actual:"" expected:"line2"） |
| 修复轮闭环 | 4 批全部红→绿；**冒烟运行发现审查与编排者基准双漏的 F20（raw string 正则被 `)"` 提前截断，主程序启动即 abort）**并当场入循环修复；不知情复审 8 条全部为修复前已存在（多属未表态项），**无修复引入的新问题**——闭环干净 |

### 6.4 由本轮反哺的 skill 进化

1. **魔数信号误报修复**：修复轮自己新写的 `inline constexpr` 具名常量被魔数信号误报
   ——CONST_DECL_RE 不认识 inline/static/extern 前缀。已修并夹具回归验证。
2. **Wrapper-bypass 提示补充**：信号把封装模块自身实现（util.cpp）的时钟调用也计入
   来源数，判断前需剔除——已在输出与文档注明。
3. **F2 门措辞强化**：基线必须记录**分节计数**（本轮编排者首次只记了 LOC 一项，
   复扫对比时缺魔数/红旗基线）。
4. **G4 派发计划核对**：性能维度被计划遗漏——由 subagent 的 out_of_scope 兜底发现；
   编排者在 G4 应对照 §3 的九个维度清单核对派发覆盖。

---

## 7. 自我进化轮 3（第三轮端到端验证：解析器类别 + 既有测试框架分支）

### 7.1 设计与执行

- **开发**：4 轮迭代实现 `eval/fcalc/`（电子表格公式引擎：递归下降解析、值模型、
  中央错误工厂、范围与聚合函数）——**D3 为函数体内破坏性语义变更**（跨类型比较
  规则改写 + 既有测试按新语义重写）、**D4 强制修改全部错误创建点**（Error 增加
  detail）；项目从 D1 起自带测试 harness（验证 TDD-LOOP 的"既有框架"分支）。
  最终 36→41 组测试、324 断言全绿。
- **审查**：3 批并行（正确性/错误处理、**性能专批**【补 R2 缺口】、一致性+构建），
  24 条原始 → 聚合 14 条（1 Major + 10 Minor + 3 Nit）。性能专批以完整调用链证实
  "每次求值重复 parse、无缓存"为 Major（与编排者基准互相印证）。
- **修复**：3 批 TDD——批 A（解析深度上限 + 字面量溢出）：红为断言失败与 SIGSEGV
  探针（500 层在 8MB 栈不崩，10 万层崩——如实记录而非造红）；批 B（解析缓存 +
  `^`/一元链深度计数）：红为**两次真实段错误**，且发现仅 LParen 计数不够、
  `^` 递归需独立计数点；批 C（text_util 合并 + -0 显示）。既有框架直接使用，
  全程零新依赖。
- **收尾**：不知情复审 3 条——**无修复引入的新问题**；1 条为复审把未表态的既有
  F4（try_parse_number 的 inf/nan/0x 盲区）以更强证据重提（实测 NaN 使 =A3=5
  为 TRUE），留用户决定；缓存无淘汰增长为已声明取舍。

### 7.2 结果与进化

| 指标 | 结果 |
|---|---|
| 查准 | 抽查全部成立；性能批每个发现都给出调用链与确定性成本论证（无需测量即判定，符合 §6） |
| 查全 | 编排者基准中的 parse-per-eval 被性能专批证实并深化；本轮无显著遗漏（性能维度已补派） |
| 修复闭环 | 4 个真红（2 次段错误 + 2 次断言失败）、41 组全绿、零新依赖、既有框架直接使用 |
| skill 进化 | ① TODO 扫描去掉 XXX 关键词——"#XXX" 错误文本约定注释被误报为 TODO（本轮实测发现）；② 已知限制/取舍必须作为 finding 记录（F21 残留 `%` 链入口、缓存无淘汰），不能只留在 (c) 说明里 |

### 7.3 三轮纵向对比

| | R2（logpipe） | R3（fcalc） |
|---|---|---|
| 项目类别 | 多线程采集管道 | 单线程解析/求值引擎 |
| TDD 分支 | 无测试设施→批准搭 harness | 既有框架直接使用 |
| 审查超出基准 | failbit Major 等 5 项 | 解析栈溢出注入、strtod 盲区、性能专批 |
| 修复闭环 | 冒烟发现双漏的 F20（regex abort） | 长链段错误红证据 + F21 残留如实上报 |
| 进化产出 | §0 执行门、硬校验、G4 核对 | TODO_RE 修复、取舍入档规则 |

---

## 8. 遵守性专项轮（第四轮：调整子代理智能程度后）

### 8.1 设计

用户调整子代理智能程度后，验证重点从"发现质量"转为**流程遵守性**：每个被派发的
agent 对照派发契约逐项打分——C1 返回规定格式 JSON（可解析）、C2 files_read+not_checked
齐备、C3 finding 字段齐全、C4 范围纪律（越界入 out_of_scope 不展开）、C5 TDD 证据
结构（红/绿 + (a)(b)(c) 契约）、C6 文件修改边界。

对象：`eval/base64c/`（Base64 编解码器，新类别：编解码/位操作库）——D1 开发（93 断言）
→ D2 迭代（MIME + 宽松空白，encode/decode 函数体内改动，117 断言）→ 2 个审查批 →
1 个修复批（TDD）→ 不知情复审。

### 8.2 遵守性计分

| 派发 | C1 格式 | C2 覆盖声明 | C3 字段 | C4 范围 | C5 TDD 证据 | C6 修改边界 |
|---|---|---|---|---|---|---|
| D1 开发 | — | — | — | ✅ 未加载 skill/未跑多余工具 | — | ✅ |
| D2 迭代 | — | — | — | ✅（诚实自报自修的 2 处缺陷） | — | ✅ 函数体内改动 |
| 审查批 1 | ✅ | ✅ | ✅ | ✅（demo 打印/RFC 差异正确归位） | — | — |
| 审查批 2 | ✅ | ✅ | ✅（1 处路径笔误已自行更正） | ✅（RFC 域常量排除、string_view 按反噪音不报） | — | — |
| 修复批 | — | — | — | — | ✅（回滚→红→恢复→绿，诚实披露"早前派发已执行"） | ✅ 逐字节还原一致 |
| 复审 | ✅ | ✅ | ✅ | ✅ | — | — |

**遵守性：7/7 个 agent 全部通过核心条款**；1 处路径笔误为唯一瑕疵（已自纠）。
发现质量同时未退化：审查抓到 MIME 位置契约违反（正是迭代引入的函数体内改动）、
截断 position 口径不一致+越界、类型命名风格分裂；不知情复审抓到**三方（两批审查+
编排者基准）均遗漏的真 Major**——解码状态机接受 ≥3 个 '=' 的非法填充（"A==="、
"====" 静默解码），属修复前已存在，按闭环规则记录为待用户决定项。

### 8.3 结论

执行门 + 返回契约 + 检查点结构对智能程度调整后的子代理依然构成有效约束：
流程遵守性 7/7，发现质量无退化，且复审环再次证明其独立价值（补住所有批次与
编排者基准的共同盲区）。

---
