---
name: cpp-review-loop
description: Review and audit C++ codebases, optimized for large-scale projects (many modules, long-lived code, 100k+ LOC), delivering an interactive HTML report whose per-finding feedback drives the follow-up fix round. Use whenever the user asks to review, check, audit, or assess C++ code quality — whole-repo review, module/subsystem review, pre-merge/PR/diff review, "帮我审查代码", "代码审查", "检查这个 C++ 项目的质量", "find bugs and code smells", architecture or maintainability assessment — even if they don't say "large scale" and even for small C++ codebases. Also use for the follow-up round: "处理审查反馈", "按反馈修复", "根据审查意见修正", or whenever a code-review-report-feedback.json / code-review-findings.json produced by this skill is mentioned. Not for writing unrelated new C++ features.
---

# Large-Scale C++ Code Review

Review C++ like a senior engineer on a large team: map before judging, verify every finding by reading its context, and separate systemic patterns from one-off issues. In a large codebase the highest-value output is the *systemic pattern* ("raw owning `new` in 34 places across 9 modules"), not a pile of per-line nits. False positives destroy trust faster than missed issues — never report a problem you have not confirmed in context.

## 0. 执行门与自检（防遗漏——每个门都有必须实际存在的产出物）

跳步是执行失败的头号原因。下表是硬性顺序：**上一道门的产出物不存在，就禁止进入下一道门**。
产出物可以是文件、命令输出或写进最终报告的一句话——缺失即该门未通过。

### 审查轮

| 门 | 动作 | 产出物（必须实际存在） |
|---|---|---|
| G1 分诊 | 确定 scope（整仓/模块/diff/单文件，§1） | 一句话范围声明（进入最终报告） |
| G2 建图 | 运行 hotspots.py（§2） | 扫描输出（保留数字供引用） |
| G3 读清单 | 按 §3 读 references/checklist.md + consistency.md + standards.md 的相关节 | 本轮启用的维度清单（写进派发表） |
| G4 派发计划 | 列批次：维度 × 文件清单（§2 执行模型） | 批次表（每批 ≤ ~15 文件） |
| G5 逐批执行 | 每批 subagent 返回后过 §2 第 4 条检查点 | 每批抽查记录 + 去重合并后的 findings 累积 |
| G6 findings JSON | 写 `code-review-findings.json`（schema：references/report-format.md） | JSON 文件——make_report.py 会**硬校验**，缺字段直接拒绝渲染 |
| G7 渲染 | make_report.py（§7） | HTML 路径（把报告与 findings 两个路径告知用户） |
| G8 摘要 | 按 §6 格式输出聊天摘要 | 含"没覆盖什么"的覆盖声明 |

### 修复轮（触发："处理审查反馈" / 用户给出 feedback JSON）

| 门 | 动作 | 产出物 |
|---|---|---|
| F1 读反馈 | 解析 feedback JSON（§7） | 四类 verdict 的分组清单 |
| F2 基线 | 跑 hotspots.py 快照（§7 第 5 步） | 基线的**分节计数**（red flags / 魔数 / 封装来源 / 文件与 LOC），逐节记录备对比 |
| F3 逐批 TDD | 每批红→绿 subagent（§7 第 6-7 步） | 每批红绿证据 + 检查点 (a)-(f) 记录 |
| F4 复扫 | 每批检查点含被改文件复扫对比基线（第 8 步 (f)） | 对比结果（新候选当场归批解决） |
| F5 复审 | 全项目复扫 + 不知情复审（第 9 步） | 复审结论（新问题：无 / 已修 / 留用户取舍） |
| F6 汇报 | §7 第 11 步 | 修复清单（含测试）/ 跳过 / 待讨论 / 复审结果 |

### 报告前自检（逐项回答 yes；任何一项 no，回到对应门补做）

- [ ] 引用了 hotspots.py 的具体数字，而不是凭印象？
- [ ] 每条发现都亲自打开过上下文核实？
- [ ] findings JSON 每条都有 file/title/detail/fix，且 make_report.py 渲染成功？
- [ ] 覆盖声明写明了"没覆盖什么"？
- [ ] 系统性模式单独成节，未与单条发现重复？
- [ ] Nit ≤ 10，且全部是项目自身惯例的偏离？
- [ ] 修复轮：每批有红绿证据、复扫对比、依赖核查 (e)？收尾做过不知情复审？

## 1. Triage the request

Determine scope and the user's concern (correctness? maintainability? performance? everything?) before reading code. Infer from the request; ask only if genuinely ambiguous:

| Scope | Typical signals | Strategy |
|---|---|---|
| Whole repo | "review this project", repo path given | Full pipeline in §2–§4 |
| One module/dir | module or subsystem named | Map the module's boundaries first, then review it against its public interface and its callers |
| Diff / PR / commit | branch, commit, patch, "review my changes" | Review changed lines plus how they are used elsewhere: grep callers of changed symbols, check touched invariants, then apply the same severity rubric |
| Single file / class | file path given | Read the file, its header pair, and a few call sites |

If the user gave no code path, ask for the repo path. Never review from memory or invent file paths.

## 2. Map before judging (whole-repo and module reviews)

You cannot read a million lines — build a cheap objective map first, then sample deliberately. Skipping this step produces reviews that fixate on whatever file was opened first.

1. **Build system & dependencies**: read the root `CMakeLists.txt` / `MODULE.bazel` / `meson.build`. List top-level targets, subdirectories, third-party deps (vcpkg/conan/submodules/FetchContent), compiler flags, warning levels.
2. **Layout**: where public headers live vs implementation, test layout, where generated/third-party code sits (exclude it from review but note it).
3. **Project conventions**: `.clang-format`, `.clang-tidy`, `.editorconfig`, AGENTS/README style docs. If the project has its own style config, the project's rules win over any generic style opinion — do not import your own taste.
4. **Hotspots**: run the bundled scanner for objective numbers:

   ```bash
   python <skill_dir>/scripts/hotspots.py <repo-root> --top 15
   ```

   It reports largest files, longest functions, include fan-out, TODO density, red-flag pattern counts (candidates, not findings), git churn, and style signals: header hygiene (include order/placement/guards/self-header-first), function-naming distribution, member-naming conventions, log/output field-style signals, and build-hardening flags (warnings level, warnings-as-errors, sanitizers). If Python is unavailable, approximate with `wc -l`, `grep -c`, and `git log --name-only`.
5. **Pick sample points**: public headers of 3–5 core modules (API quality), the top hotspot files, 2–3 of the worst red-flag sites, one "boring" module as baseline, and the build files.

Budget note: for a very large repo keep deep reading to roughly 20–50 files; rely on pattern sweeps for breadth. Always state what was *not* reviewed (see coverage statement in §6).

### Execution model: orchestrator + focused subagents (default)

Deep reading is delegated. The orchestrating agent never "reviews everything" in one pass — that is how attention dilutes and unverified grep hits leak into reports. If the host has no subagent capability, execute the same plan sequentially yourself with the same checkpoint discipline.

1. **Orchestrator (main agent) owns**: triage, the map (above), the dispatch plan, ALL file writes (findings JSON, HTML), and the final §6 summary. Single-writer principle: subagents return text/JSON fragments and never write files. (The fix round in §7 flips the roles: fix subagents write code, the orchestrator reviews every change.)
2. **Dispatch unit = one dimension × one file batch.** Each subagent gets a narrow scope: one review dimension (one checklist section) over ≤ ~15 files / a few thousand LOC; for module reviews, one module's boundary checks. Never "review the repo". Independent batches may run in parallel where the host allows it; consolidation stays serial.
3. **Subagent prompt contract** — adapt this template:

   > You are doing ONE focused review pass. Scope: \<dimension\> in \<exact file list\>.
   > First read `\<skill_dir\>/references/checklist.md` §\<N\> (consistency passes: also `references/consistency.md`).
   > Verify every candidate in context before reporting; a grep hit is not a finding. Severity rubric is in that checklist.
   > If you notice issues OUTSIDE your dimension, append them to an `out_of_scope` list — do not investigate them.
   > Return ONLY: (a) a JSON array of findings `{severity, category, file, line, title, detail, fix}` in the user's language; (b) `files_read`; (c) `not_checked` (coverage fragment). No prose report.

4. **Checkpoint after EVERY return (concentrated, non-skippable)** — before dispatching the next batch, the orchestrator: (a) spot-verifies at least one finding per batch by opening the cited file itself; (b) drops anything it cannot confirm; (c) dedupes against findings so far; (d) folds confirmed ones into the running findings JSON and updates systemic-pattern counts; (e) records the batch's coverage fragment. Only then dispatch the next batch.
5. **Sizing**: a 100k+ LOC whole-repo review plans roughly 4–10 batches from the map (one per dimension, split further per module where a dimension is heavy). Single-file and small diff reviews → 1–2 batches, or direct execution when trivially small.

## 3. Review dimensions

Open `references/checklist.md` for the concrete red flags per dimension, including what NOT to flag. On any given review, weight the dimensions by what the map surfaced (e.g. heavy churn + mutexes → concurrency first) rather than trying to check everything equally:

- Architecture & module boundaries (layering, dependency direction, circular includes, god objects, singletons)
- Headers & API design (header hygiene, ABI-aware API, non-virtual destructors on polymorphic bases, `explicit`, deprecated APIs)
- Ownership & resource management (RAII, raw owning pointers, smart-pointer choice, non-memory resources)
- Concurrency (guarded invariants, lock ordering, lifetimes of threads/async work, atomics memory order)
- Error handling (mixed styles, exception safety, `noexcept` correctness, unchecked error returns)
- Performance (allocation in hot paths, copies vs moves, data-structure choice) — mark suspicions as "needs measurement" unless profiling evidence exists
- Build & dependency hygiene (per-target vs global settings, warnings level & warnings-as-errors, hardening/sanitizer options, vendored duplicates, non-hermetic includes)
- Consistency & C-vs-C++ style audit — measure the project's dominant convention first, then flag deviations: naming uniformity (members/functions), include order & guards, typedef and ctor-init habits, log/output format uniformity (`key=value` styles, field renames, printf spec mismatches — §L), wrapper-bypass consistency (time / rand / CRC — §M), C-isms (`void*` generics, C-style casts, memset on objects). Read `references/consistency.md` for the measure-then-flag method and the full item list.
- Testing & CI signals (what is testable, what is not, and why)

Items adopted from public standards — Core Guidelines (ownership, class rules), Google (lambda capture discipline, `explicit`/`override`), CERT (numeric & exception safety) — live in `references/standards.md` §F–§I. They map onto the dimensions above; review them as part of those dimensions, not as a separate pass.

## 4. Evidence rules

- Every finding needs: `file:line`, what is wrong, why it matters here, and a concrete suggested fix.
- Red-flag greps and the hotspot script produce **candidates**. Open the file and read the surrounding code before reporting; a grep hit is not a finding.
- Before claiming UB / a race / a leak, re-check the claim once more against the actual code (e.g. is that "raw new" actually handed to a smart pointer three lines later?).
- Severity rubric:
  - **Blocker** — memory unsafety, UB, data race, deadlock, crash path, security hole.
  - **Major** — likely correctness bug, leak, ownership ambiguity, systemic maintainability damage.
  - **Minor** — robustness risk, suspected performance issue, API sharp edge.
  - **Nit** — style/local naming; list only deviations from the project's own config or dominant convention (see references/consistency.md), capped at ~10.
- Prefer 15 verified findings over 60 noisy ones. If a pattern is already listed under systemic patterns, don't repeat every instance in the detail section.

## 5. Report systemic patterns separately

For each repeated issue, aggregate: name the pattern, give the count and where it concentrates, list 2–3 example locations, and give ONE fix recommendation that addresses the class of problem. Example:

> **Raw owning `new` outside factories — 34 sites in 9 modules** (worst: `src/net/pool.cc:142`, `src/db/row.cc:88`, `src/util/buffer.cc:51`). Ownership of the returned object is documented nowhere and two sites delete with a different type than they new. Recommendation: route creation through `std::unique_ptr` factory functions module by module, starting with `net`.

## 6. Report format

Write the report in the language the user used (Chinese request → Chinese report); keep code identifiers, paths, and quoted comments as-is. Structure:

```markdown
## 总体评估
（3–5 句：审查范围与覆盖度、整体健康度、最需要关注的 3 件事）

## 发现汇总
| 严重度 | 位置 | 问题 |
|---|---|---|
| Blocker | src/net/pool.cc:142 | ... |

## 系统性模式
（每条：模式名、数量与分布、2–3 个示例位置、一条修复建议）

## 重点问题详情
（只展开 Blocker / Major。每条格式如下）

### [Major] threads/worker_pool.cc:214 — 回调在持锁状态下执行
Worker::on_message 在持有 `mutex_` 时调用用户注册的回调；回调若再进入
任何加锁接口（如 `submit()`）就会死锁。
建议：先把回调和数据拷贝出来，释放锁后再调用（可参考 src/scheduler 的处理方式）。

## 本次审查覆盖了什么 / 没覆盖什么
（覆盖：构建体系、12/85 个模块的公共头文件、20 个热点文件；未覆盖：其余实现细节、运行时行为）

## 建议的后续步骤
（按性价比排序：先低成本止血，再结构性改造）
```

The coverage statement is mandatory for anything larger than a single-file review — it sets honest expectations and is what separates a professional review from a lucky skim.

Keep the chat message at summary level (总体评估 + 发现汇总表); the full per-finding detail goes into the interactive HTML report (§7) — don't paste the entire detail section into chat.

## 7. Interactive HTML report & feedback loop

Every review run ends with an interactive HTML report the user annotates; their feedback drives the fix round.

**First run (producing the report):**

1. Write the findings as JSON to `<reviewed-repo-root>/code-review-findings.json` — schema and field rules in `references/report-format.md` (review meta + coverage, summary, systemic patterns, one record per finding with `id/severity/file/line/title/detail/fix/category`, in the user's language).
2. Render it: `python <skill_dir>/scripts/make_report.py code-review-findings.json` → writes `code-review-report.html` next to it. Tell the user both full paths.
3. The chat message stays at §6 summary level; the HTML carries the detail.
4. The user opens the HTML in a browser, sets 采纳修复 / 驳回 / 待讨论 / 自定义 per finding (optional comment), and clicks 导出反馈 JSON — the browser saves `code-review-report-feedback.json` next to the report (a 复制反馈 fallback exists). Page state autosaves to localStorage, so a refresh loses nothing.

**Feedback round (fixing):** triggers — "处理审查反馈" / "按反馈修复" / "根据审查意见修正", or the user names/pastes the exported feedback JSON (`code-review-report-feedback.json`).

Verdict semantics:

1. Read the feedback JSON. Each verdict entry carries a snapshot (file, line, title, fix) — trust the snapshot even if the findings JSON has moved or is gone.
2. `reject` → skip it; carry the user's rationale into the final summary instead of arguing.
3. `discuss` → respond to the comment; do not change code. A `discuss` verdict can mean the user picked 待讨论 *or* only wrote an opinion without picking a verdict (comment-only items export as discuss) — unless the comment is an explicit instruction, address it in words, not in code.
4. `accept` → implement the fix, adjusted by the user's comment if any. `custom` → the comment IS the decision: the user specified their own resolution (e.g. "函数名统一大驼峰"). Implement exactly what the comment says — it overrides the original suggested fix. An empty comment degrades to discuss.

Fix execution — the §2 orchestration discipline with roles flipped: **fix subagents write code, the main agent reviews every change**, and every behavioral fix follows **TDD-LOOP: test first (red) → fix (green) → verify**, the test kept as permanent regression coverage:

5. **Plan batches**: default one finding per batch; merge findings into one batch only when they touch the same function/file and a single edit serves both. Order batches so dependencies are fixed first and each diff stays minimal. Split findings into behavioral (logic / robustness / API contracts — unit-testable) and cosmetic (naming / style / header splits / build flags — not unit-testable); TDD-LOOP is mandatory for the former. Before the first batch, run `hotspots.py` and keep its per-file section counts as the **pre-fix baseline** — every later checkpoint compares against it.
6. **Test first (TDD-LOOP, mandatory for behavioral fixes)**: each behavioral batch starts with a unit test that reproduces the finding, written into the project's EXISTING test location and framework; run it — it must FAIL on the current code (red). A test that already passes does not reproduce the finding — adjust the test, not the code. If the project has no test infrastructure, STOP and ask the user whether to bootstrap a minimal one (offline constraint: a dependency-free harness or the build system's native test support — never fetch a framework from the network); proceeding test-less requires the user's explicit OK. Cosmetic fixes skip test-first but must leave the existing suite green.
7. **Dispatch** — one fix subagent per batch, adapting this template:

   > You are implementing ONE review fix, TDD-LOOP style. Files: \<exact paths\>. Finding: \<title — detail — original fix\>. User comment (overrides the original fix if it says how): \<comment\>.
   > Steps: (1) write a unit test covering this finding's scenario in the project's existing test location/framework — reuse its framework, do NOT introduce new libraries; run it — it must FAIL now; a passing test means it does not reproduce the finding, so fix the test first. (2) Implement the fix with a minimal diff — no drive-by refactors, no new #includes or linked libraries beyond what the fix strictly requires, match the file's existing style, do not touch other findings. (3) Re-run the test until it passes, then run the surrounding existing tests for regression. If the fix proves wrong or unsafe in context, STOP and report back instead of improvising.
   > Return ONLY: (a) test file + test name + red/green evidence (the actual failure output before, the pass after); (b) per-file summary of the exact changes made; (c) anything intentionally not done and why. No prose report.

8. **Checkpoint after EVERY batch (non-skippable — the main agent is the reviewer)**: re-read the changed region, the new test, and their callers; verify (a) the test genuinely covers the finding — would it fail if the fix were reverted?; (b) the change implements the finding as adjusted by the user's comment; (c) nothing new broke — touched lines re-checked against the relevant checklist red flags, file conventions intact, adjacent behavior untouched; (d) the red→green evidence is real — re-run the test yourself when the suite is fast; (e) the diff introduced no unnecessary dependencies — diff the `#include` blocks, CMake `target_link_libraries`, and test framework usage against the pre-fix state: anything new must be strictly required by THIS fix (new third-party or test frameworks violate step 6's offline/existing-framework rule — reject and re-dispatch); (f) **re-run `hotspots.py` on the touched files and diff the counts against the pre-fix baseline** — any NEW candidate hits introduced by this diff (a new magic number, raw `new`, `catch(...)`, a new time source…) are findings of THIS batch: resolve them here, not in a later pass. Fail → fix directly if trivial, otherwise re-dispatch a correction batch and re-check. Only then dispatch the next batch.
9. **Final re-review (loop closure)**: when all batches pass, do NOT report yet. Re-run `hotspots.py` project-wide and compare totals against the pre-fix baseline (step 5) — the fix round must not have made the numbers worse. Then dispatch fresh-eyes subagent(s) over ALL touched files using the §2 review discipline (dimension batches; give the file list but do NOT reveal the fix intentions) — intent-blind review catches what familiarity hides. Compare its findings against what was just changed: pre-existing issues → note them for the user; NEW issues introduced by the fixes → send them back through steps 5–8. Iterate until the re-review is clean or only user-decision tradeoffs remain.
10. **Build/tests**: each behavioral batch already runs its own red→green test (steps 6–8); run the project's full existing build/test suite before the final report, and only if it is already configured. Never fetch third-party tooling on your own; bootstrapping a minimal test harness requires the user's OK (step 6).
11. **Final message**: fixed list (each with file:line AND its new/updated test), skipped list with reasons, open discussions; re-review outcome (new issues: none / fixed / left as user tradeoffs); remind the user if `decided < total_findings` (some findings were never marked).

## 8. Optional tooling

Static analyzers add value but only if already installed. Read `references/tooling.md` before running any of them. Rules: never install tools into the user's project to enable a review, never modify project files, and always prefer the project's own `.clang-tidy` configuration over inventing a check list. If tooling is unavailable, skip it and say so in the coverage statement.
