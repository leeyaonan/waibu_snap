# 自动化测试

使用 Qt Test + CTest，Python 3 仅运行测量计算单测（标准库）。构建后执行成对的 `test-project` 脚本。

| CTest 名称 | 覆盖 |
| --- | --- |
| `startup_smoke` | 真正应用子进程加载动态库、进入事件循环；关闭临时窗口后仍驻留，触发实际托盘“退出”动作后正常退出；真实覆盖层首帧绘制、Retina 反向拖选、Esc 取消；未显式启用测试模式时受控参数拒绝 |
| `selection_geometry` | 半开像素矩形、任意拖选方向、边缘取整、夹取、零面积与无效缩放；未完成计时不能伪造可交互终点 |
| `probe_cpu_calibration`（macOS） | libproc Mach 时间换算对照 getrusage CPU 微秒时间；原错误在 M3 Pro 上先失败，修正后通过 |
| `measurement_tool` | nearest-rank 30 次第 29 项、最大样本和空样本；无效会话过滤；刷新代理不能宣布通过；跨进程 CPU 总和、进程退出后的记账缺口 |

`startup_smoke` 使用 `QT_QPA_PLATFORM=offscreen`，验证 QWidget / 菜单动作与生命周期；无头环境不显示系统托盘，也不注册真实热键或申请屏幕录制权限。实际应用执行 `--smoke-test` 时检查驻留策略、菜单结构、临时窗口显示及关闭、事件循环持续运行，再触发“退出”动作；退出断言有进程超时与状态检查。`--smoke-test` 仅 `BUILD_TESTING=ON` 可用，不能把关闭最后窗口即退出当作托盘应用行为。

纯逻辑单测不代表真实 SCK 捕获、系统托盘、Carbon F1、DPR 像素内容或物理呈现验收；相应 MAC-01 彩排与缺口见 [脚本说明](../scripts/README.md)。Windows 桩参与产品与测试编译，尚未实现 Windows 捕获行为。
