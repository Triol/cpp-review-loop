# Report & Feedback Formats — HTML 交互报告闭环

审查的可交付物是一个**可交互 HTML 报告**。数据流：

```
审查分析 → code-review-findings.json（agent 写）
        → make_report.py 渲染 → code-review-report.html（用户打开）
        → 用户逐项标注 + 导出 → code-review-report-feedback.json（浏览器生成）
        → agent 读取 feedback → 执行修复 → 汇总
```

三个文件默认都放在**被审查仓库的根目录**（或用户指定的位置），是临时产物，修完可删。命名约定固定：findings 文件任意名，报告默认 `code-review-report.html`，反馈文件 = 报告名去掉 `.html` 加 `-feedback.json`（由渲染脚本注入 HTML，浏览器导出时使用该名字）。

## 1. findings JSON（agent 写）

```json
{
  "review": {
    "repo": "F:/path/to/repo",
    "scope": "whole repo",
    "coverage": "覆盖：构建体系、12/85 模块公共头、20 个热点文件；未覆盖：其余实现细节"
  },
  "summary": "总体评估，3–5 句：整体健康度 + 最需要关注的 3 件事",
  "systemic": [
    {
      "title": "Raw owning new outside factories",
      "count": 34,
      "examples": ["src/net/pool.cc:142", "src/db/row.cc:88"],
      "recommendation": "一条修复建议，解决这一类问题"
    }
  ],
  "findings": [
    {
      "id": "F1",
      "severity": "Blocker",
      "category": "concurrency",
      "file": "src/net/pool.cc",
      "line": 142,
      "title": "回调在持锁状态下执行",
      "detail": "问题事实与为什么重要（1–3 句）",
      "fix": "具体修复建议"
    }
  ]
}
```

字段规则（`make_report.py` 会**硬校验**，缺任一项将拒绝渲染并列出中文报错）：

- **必填**：`review.scope`、`review.coverage`（必须同时写明覆盖与未覆盖）、`summary`；
  每条 finding 的 `file`、`title`、`detail`、`fix`。这是防遗漏门槛——补齐才会渲染。
- `severity` ∈ `Blocker | Major | Minor | Nit`（大小写不敏感，脚本会归一化；未知值按 Minor）。
- `id`：建议显式写（`F1..Fn`，与发现汇总表一致）；省略时脚本自动编号。`systemic` 每项同样建议带 `id`（`S1..Sn`，省略时自动编号）。页面上编号徽章按严重度着色、点击即复制（悬停显示「点击复制」小字提示），用户复制编号（如 `F9`）即可与 agent 精确对话。
- 文本用**用户的语言**；`detail`/`fix` 里可以含 `</` 等字符（渲染脚本会转义）。
- 想让用户对某个系统性模式表态时，把它同时列成一条 finding（`file` 填 `multiple`，`line` 省略）。

## 2. 渲染

```bash
python <skill_dir>/scripts/make_report.py code-review-findings.json
# 可选 --out 指定 HTML 路径；默认写到 findings 同目录
```

脚本打印 report / feedback / findings 三个路径——把 report 的完整路径告诉用户。

**离线约束（硬性）**：报告必须保持完全自包含——模板不引 CDN、不引网络字体（只用系统字体栈）、无 `<script src>`/`<link>`/`<img>` 外链、无 fetch/XHR；`make_report.py` 与 `hotspots.py` 仅用 Python 标准库（无需 pip 安装，git churn 只做本地仓库操作）。改动模板或脚本时保持零外部依赖，保证在完全断网的机器上可渲染、可交互、可导出。

## 3. feedback JSON（浏览器导出，agent 读取）

用户在页面上对每项设置 采纳修复 / 驳回 / 待讨论 / 自定义（可加意见），点「导出反馈 JSON」下载（剪贴板不可用时页面提供手动复制兜底）。结构：

```json
{
  "report": "code-review-report.html",
  "findings_file": "F:/path/to/repo/code-review-findings.json",
  "repo": "F:/path/to/repo",
  "exported_at": "2026-08-30T12:34:56.789Z",
  "total_findings": 12,
  "decided": 9,
  "verdicts": {
    "F1": {
      "verdict": "accept",
      "comment": "同意，但注意顺带更新调用方 src/a.cc:77",
      "severity": "Blocker",
      "file": "src/net/pool.cc",
      "line": 142,
      "title": "回调在持锁状态下执行",
      "fix": "…报告里的原建议…"
    }
  }
}
```

- `verdict` ∈ `accept | reject | discuss | custom`；`discuss` 含"只填意见未点表态按钮"的项（agent 回应意见、不改代码）；`custom` = 用户意见即决策/方案，agent 按意见执行（意见为空按 discuss 处理）。`decided` = 已表态或已写意见的项数，可能与页面上"已点按钮"的计数不同。
- 每条自带 finding 快照——即使 findings JSON 被移动/删除，修复回合也能只靠 feedback 文件工作。

## 4. 修复回合（agent 读取 feedback 后）

判决语义：`accept` = 按快照实现修复（结合用户 comment）；`reject` = 跳过并把理由写进汇总；
`discuss` = 只回应意见不改代码；`custom` = 意见即决策，严格按意见执行（覆盖原 fix 建议；
意见为空按 discuss 处理）。

执行编排（subagent 派发、每批一个 finding、主 agent 逐批检查点、收尾不知情复审的
fix-verify 闭环）**以 SKILL.md §7 为准**，此处不重复，避免两处文档漂移。汇总要求：
已修复清单（每条 file:line）、跳过清单（附原因）、待讨论项；`decided < total_findings`
时提醒用户；项目有自己的构建/测试且已配置好时才运行验证。
