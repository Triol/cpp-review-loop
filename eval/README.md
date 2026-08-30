# eval/ — 有效性实验原始产物

本目录是 [docs/VALIDATION.md](../docs/VALIDATION.md) 所述验证实验的**原始产物**，
公开在此以便读者核对实验证据、自行复现测量。

## task-scheduler/

端到端双盲实验（见 VALIDATION.md §1）中由独立子 agent 实现、并经 **5 轮迭代演化**
的"被审查项目"——一个刻意像真实团队一样持续演化的进程内任务调度器：

- 初始版 954 行 / 10 文件（双盲审查实验的审查对象，查准 8/8、误报 0 的实证）；
- 迭代 1–3：重试、周期任务、日志统计报表、worker 字段（催生日志格式漂移的实证）；
- 迭代 4–5：CSV 导出、trace 追踪、死信队列（催生函数膨胀与结构性重复的实证）。

其中包含：

| 文件 | 说明 |
|---|---|
| `include/`、`src/`、`CMakeLists.txt` | 被审查项目本体（**注意：这是故意保留演化瑕疵的代码，不是学习范例**） |
| `code-review-findings.json` | 双盲审查实验产出的发现清单（8 条） |
| `code-review-report.html` | 对应的交互式审查报告（可直接浏览器打开） |

## 复现测量

```bash
# 最长函数排名 / 头文件组成 / 日志键值风格 / 构建加固旗标
python cpp-review-loop/scripts/hotspots.py eval/task-scheduler --top 15
```

各轮迭代的具体需求提示词与测量数据记录在 VALIDATION.md §1、§3、§3.5。
