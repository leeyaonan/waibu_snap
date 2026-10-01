# 自动化测试

使用 Qt Test + CTest。运行 `scripts/test-project.sh` 或 `scripts/test-project.ps1`（先构建），完整命令见仓库 README。

CTest 登记 `startup_smoke`，通过 `QT_QPA_PLATFORM=offscreen` 在无头环境加载 Qt Widgets。它包含两个双端用例：实际窗口可以显示并关闭；真实应用子进程能够启动、进入事件循环、关闭最后窗口并以 0 退出。后者使用仅在 `BUILD_TESTING=ON` 时编译的 `--smoke-test` 入口，并设置进程和 CTest 超时，避免挂住 runner。

| 目录 | 层次 | 当前状态 |
| --- | --- | --- |
| `unit/` | 核心纯逻辑 | 预留，无产品逻辑 |
| `integration/` | 跨模块集成 | `startup_smoke.cpp`：窗口及进程生命周期 |
| `e2e/` | 产品端到端 | 预留，不把无头测试视为截图 / 热键 / DPI 验收 |
| `fixtures/` | 测试素材 | 预留 |

用例标识使用可移植的英文 C++ 标识符，场景描述、失败提示和注释使用中文。回归缺陷先补失败用例再修复；新增平台用例须注明适用端。Qt Test 不链接到产品可执行文件。
