# AGENTS.md — WaibuSnap 源码仓库

> 本仓库只放**源码与工程配置**；产品文档、愿景、设计资料位于本地工作区 `waibu_snap_workspace`（独立 git，不上传）。
> 工作区完整协作规约见 `../AGENTS.md`，本文件只列源码仓库的核心约束。

## 核心约束

1. **禁止向 `main` 直接推送**。唯一例外（仅一次）：仓库首次播种（bootstrap）时允许推送本地 `main` 建立初始历史；此后所有改动走「分支 → push → PR → 合并」。
2. 功能分支必须基于最新 `main` 创建，upstream 关联**同名远程分支**（`git push -u origin <分支名>`），禁止 track 到 `main`；push 前核对 `git rev-parse --abbrev-ref --symbolic-full-name @{u}`。
3. 所有沟通、文档、代码注释、提交信息一律使用中文；提交信息格式 `<type>: <中文描述>`（type ∈ `feat | fix | docs | refactor | test | chore | build | ci`）。
4. 需求与决策在工作区维护（规格 `../docs/specs/`、决策记录 `../docs/adr/`），实现前先读对应规格；重要技术取舍在合并前补记 ADR。
5. 不伪造测试/运行结果；不确定的 API、平台行为先查证或标注「待验证」。
6. 文本统一 UTF-8 + LF（Windows 专用脚本除外，CRLF）；不硬编码路径分隔符与机器特定路径。
