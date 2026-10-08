# AGENTS.md — WaibuSnap 源码仓库

> 本仓库只放**源码、工程配置与应用静态资源**；应用图标及再生成说明位于 `assets/icons/`，菜单栏 / 托盘图标位于 `assets/icons/tray/` 并通过 Qt 资源系统嵌入；产品文档、愿景、设计资料位于本地工作区 `waibu_snap_workspace`（独立 git，不上传）。
> 工作区完整协作规约见 `../AGENTS.md`，本文件只列源码仓库的核心约束。

## 核心约束

1. **禁止向 `main` 直接推送**。唯一例外（仅一次）：仓库首次播种（bootstrap）时允许推送本地 `main` 建立初始历史；此后所有改动走「分支 → push → PR → 合并」。
2. 功能分支必须基于最新 `main` 创建，upstream 关联**同名远程分支**（`git push -u origin <分支名>`），禁止 track 到 `main`；push 前核对 `git rev-parse --abbrev-ref --symbolic-full-name @{u}`。
3. 所有沟通、文档、代码注释、提交信息一律使用中文；提交信息格式 `<type>: <中文描述>`（type ∈ `feat | fix | docs | refactor | test | chore | build | ci`）。
4. 需求与决策在工作区维护（规格 `../docs/specs/`、决策记录 `../docs/adr/`），实现前先读对应规格；重要技术取舍在合并前补记 ADR。
5. 不伪造测试/运行结果；不确定的 API、平台行为先查证或标注「待验证」。
6. 文本统一 UTF-8 + LF（Windows 专用脚本除外，CRLF）；不硬编码路径分隔符与机器特定路径。

## 代码规范

- C++17；macOS 原生桥接使用 Objective-C++17，Windows 使用 MSVC v143（VS 2022，或 VS 2026 宿主加装 14.44 工具集）。仅支持 Qt 6.11.2 动态库。
- `.clang-format` 采用 LLVM 基础风格、4 空格、100 列、Allman 大括号；统一 clang-format 18.1.8。理由是沿用成熟规则并保持 Qt 多层调用清晰，避免双端手工排版分歧。
- 类名 PascalCase，函数 / 变量 camelCase，文件名 snake_case，命名空间 `waibusnap`；测试标识使用英文，注释与场景说明使用中文。
- 资源优先 RAII；QObject 明确父对象 / 所有权；共享接口不暴露原生句柄。平台代码只放 `src/platform/macos/`、`src/platform/windows/`，契约放 `src/interfaces/`。
- 应用只用 Qt Core / Gui / Widgets；Qt Test 仅供测试。禁止 GPL-only 模块；新增依赖须先记录用途与许可证并遵循工作区审批约定。
- 编译开启严格警告；提交前运行构建、CTest 与格式检查。未具备工具时明确记录限制，不把静态检查当作构建通过。

## 常用命令

在源码仓库根目录运行；设置 `QT_ROOT_DIR`，详见 [README.md](README.md) 与 [脚本登记](scripts/README.md)。Windows 终端使用 PowerShell 7。

| 动作 | macOS | Windows |
| --- | --- | --- |
| 环境检测 | `bash scripts/prepare-environment.sh` | `./scripts/prepare-environment.ps1` |
| 构建 | `bash scripts/build-project.sh` | `./scripts/build-project.ps1` |
| 测试 | `bash scripts/test-project.sh` | `./scripts/test-project.ps1` |
| 运行 | `bash scripts/run-app.sh` | `./scripts/run-app.ps1` |
| 格式检查 | `bash scripts/lint-code.sh` | `./scripts/lint-code.ps1` |
| 打包 | 本轮未实现，构建树不是分发包 | 本轮未实现，构建树不是分发包 |

脚本只检测已有环境，不擅自安装 Qt。CI 双矩阵负责 Qt 6.11.2 安装、configure、build、ctest 与格式检查；CI 通过不等于 V01–V03 实机验收通过。
