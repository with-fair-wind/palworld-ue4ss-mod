# docs

- `sdk-dependency-registry.md`：本 mod 依赖的 Palworld SDK 名称与 ABI 敏感结构清单（活文档，随代码维护）。
- `pal-individual-field-audit.md`：帕鲁个体字段的反射访问审计记录。

## 已移除：superpowers 设计与实施计划

历史上 `docs/superpowers/`（33 份 plans + 34 份 specs）存放各功能的 brainstorm→设计→实施计划。
这些过程产物的长期价值（实测教训、被否决方案、契约结论）已全部沉淀进 `AGENTS.md` 的契约与
验证清单；过程文档本身与代码的偏差会随演进持续积累（合并前已多次修正漂移），故整体移除。

历史内容永久可查：`git log -- docs/superpowers/`（2026-07 至 2026-08，含各阶段的
superseded 标注）。现行契约以 `AGENTS.md` 为准。
