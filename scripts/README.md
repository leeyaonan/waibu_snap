# 开发脚本

所有脚本按 `<动作>-<目标>` 成对提供；从任意目录调用均能定位源码根目录。`.sh` 为 UTF-8 / LF，`.ps1` 为 UTF-8 / CRLF；Windows 使用 PowerShell 7，避免 Windows PowerShell 5.1 对无 BOM UTF-8 的误读。

## 命令登记

以下路径以源码仓库根目录为基准：

| 用途 | macOS | Windows（PowerShell 7） |
| --- | --- | --- |
| 环境检测 / 安装指引 | `bash scripts/prepare-environment.sh` | `./scripts/prepare-environment.ps1` |
| configure + build | `bash scripts/build-project.sh` | `./scripts/build-project.ps1` |
| Qt Test / CTest | `bash scripts/test-project.sh` | `./scripts/test-project.ps1` |
| 启动已构建程序 | `bash scripts/run-app.sh` | `./scripts/run-app.ps1` |
| 格式检查（不改文件） | `bash scripts/lint-code.sh` | `./scripts/lint-code.ps1` |

本轮新增的性能采集入口在下节登记；Windows 入口明确非零失败（待补）。无自动下载、安装、打包或发布脚本。失败返回非零退出码；测试使用 `--no-tests=error`，避免零测试假通过。

## 手动准备与版本锁定

Qt 通过 [官方开源安装器](https://www.qt.io/download-qt-installer-oss)选择 **6.11.2**，只需桌面基础组件 qtbase（Core / Gui / Widgets / Test 及必需插件），不用 QML / WebEngine 或 GPL-only 模块。安装器可能同时展示其他组件，勿默认勾选。将 `QT_ROOT_DIR` 指向 kit 根目录。

- macOS：准备 Xcode 15+ / 命令行工具；CMake 3.24+ 与 Ninja 可由开发者自行通过官方安装包或 Homebrew 安装（例如 `brew install cmake ninja`，脚本不会执行该命令）。
- Windows：准备 Visual Studio 2022 C++ 桌面开发、Windows SDK、[CMake](https://cmake.org/download/)和 [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)。CMake 与 pwsh 加入当前用户 PATH。优先使用 VS 2022；兼容 VS 2026 宿主加装 MSVC v143 14.44 x64（需要 CMake ≥ 4.2）。脚本按已安装宿主选生成器并固定 `-T v143`，无需另装 Ninja 或手动运行 vcvars。
- 可选的本地格式工具：已有 Python 3 时，可自行创建 `.venv`，在其中运行 `python -m pip install clang-format==18.1.8`，激活后执行 lint 脚本。只在需要格式检查时安装，不是应用构建的前置条件。

环境脚本用 qmake 检查 **6.11.2**，Windows 还检查 MSVC kit；CMake `EXACT` 再次检查版本并拒绝静态 Qt。CI action、Qt、aqtinstall、py7zr 与 clang-format 均有固定版本或提交；runner 镜像和系统 SDK 随 `*-latest` 更新，详细版本以每次 CI 日志为准，不声称完整可复现构建。

## macOS 27 授权归属

macOS 27 起 TCC 按责任进程判定屏幕录制授权，终端直接执行 `.app/Contents/MacOS/WaibuSnap` 时不会继承应用的授权。真实截图必须经 LaunchServices 启动：访达、`open` 或 `run-app.sh`。首次仍需在系统设置允许 WaibuSnap，系统要求时重启；脚本不替用户授权。

`run-app.sh` 用 `open -W -n <WaibuSnap.app> --stdout <日志> --stderr <日志> --args ...` 启动，标准输出 / 错误写入 `$WAIBUSNAP_BUILD_DIR/logs/run-app.log`（默认 `build/macos/logs/run-app.log`），并由 `tail -f` 保持终端可见，阻塞到应用退出。Ctrl+C 仅停止脚本与日志跟随，应用继续驻留；请从菜单栏退出。启动前 `pgrep -x WaibuSnap` 检查已有实例，存在时说明「请先从菜单栏退出，或直接使用它」并结束。`open` 的退出状态反映启动 / 等待器状态，受控捕获是否成功仍须检查会话日志的数量、结果码与可交互终点。

## 参数

| 环境变量 | 默认 / 作用 |
| --- | --- |
| `QT_ROOT_DIR` | Qt kit 根目录；未指定时从 PATH qmake 检测 |
| `WAIBUSNAP_BUILD_DIR` | macOS 为仓库内 `build/macos`，Windows 为 `build/windows-x64`；建议自定义为绝对路径 |
| `WAIBUSNAP_BUILD_TYPE` | `Release`；也接受 Debug / RelWithDebInfo / MinSizeRel；同一轮构建、测试、运行须保持一致 |
| `MACOS_ARCHITECTURES` | 仅 macOS，默认 `uname -m`；可设 x86_64 或 `arm64;x86_64`（x86_64 已通过 CI 交叉编译，通用二进制与 Intel 实机待验证） |

macOS 配置固定部署下限 14.0，Windows 固定 MSVC v143 x64（宿主可为 VS 2022 / 2026）。脚本不申请管理员权限、不变更系统策略。自定义架构请使用不同构建目录。

Qt Test 与测量工具计算单测需要 Python 3（仅标准库，CI 已提供）。脚本始终开启 `BUILD_TESTING`。需要仅构建应用时，可手动使用相同 CMake 参数并传 `-DBUILD_TESTING=OFF`；此时既不查找 Qt Test，也不编译 `--smoke-test` 自动关闭入口。没有发布包入口，不能将该构建开关等同于发布验收。

## 直接依赖与工具登记

本次没有新增应用第三方运行时依赖；以下为已选 Qt、用户要求的工程工具及 CI 辅助工具。工具许可不用于重新许可应用。Qt 的传递依赖声明随 SDK 保留，完整发行清单在打包前核对。

| 项目 / 锁定 | 用途 | 许可 / 范围 |
| --- | --- | --- |
| Qt Core / Gui / Widgets 6.11.2 | 应用运行时，动态链接 | LGPLv3；来源 qtbase，不引入 GPL-only 模块 |
| Qt Test 6.11.2 | 测试可执行文件 | LGPLv3；不进入应用目标，[官方说明](https://doc.qt.io/qt-6.11/qttest-index.html) |
| CMake / CTest ≥ 3.24 | 工程配置、构建调度、测试 | BSD 3-Clause；开发工具 |
| Ninja | macOS 构建后端 | Apache-2.0；开发工具，runner 提供 |
| Xcode / Apple SDK，MSVC v143 / Windows SDK | 平台编译与系统头文件 | 各厂商 SDK / 工具许可；无新增应用库 |
| Python 3.14（CI） | 运行下载器与格式工具安装 | PSF；CI / 可选开发工具 |
| PowerShell 7 | Windows 脚本宿主 | MIT；开发工具 |
| clang-format 18.1.8 | 双端格式检查 | LLVM Apache-2.0 WITH LLVM-exception；PyPI 打包项目 MIT，[版本页](https://pypi.org/project/clang-format/18.1.8/) |
| aqtinstall 固定提交 `8c3695d4…` | CI 从 Qt 下载站安装指定 qtbase | MIT；仅 CI，[源码](https://github.com/miurahr/aqtinstall) |
| py7zr 1.1.0 | CI 解压 Qt 归档 | LGPL-2.1-or-later；仅 CI，不进入应用 |
| jurplel/install-qt-action（固定 SHA） | CI Qt 下载 / 缓存及环境设置 | MIT；仅 CI，[源码](https://github.com/jurplel/install-qt-action) |
| actions/checkout、actions/setup-python（固定 SHA） | CI 源码与 Python 准备 | MIT；仅 CI |

安装器自身的 Python 传递包由其上游依赖解析；这些包不打包进应用。本轮不复制第三方 SDK 二进制、源码或许可证到仓库，也不产生可分发包。

Qt 下载器说明：PyPI 的 aqtinstall 3.3.0 尚未支持 Qt 6.11 的 Windows 仓库新布局，首轮 CI 在获取 Updates.xml 时失败。因此 CI 将下载器锁定到已合入 [上游 PR #1000](https://github.com/miurahr/aqtinstall/pull/1000) 的提交 `8c3695d4a4e1ceabf6a74dc6c79681656dc6b74b`，不跟随 master。此调整仅影响 CI 安装器，Qt 仍为 6.11.2。

Windows runner 说明：`windows-latest` 在本次运行使用 Windows Server 2025 / VS 2026，包含 MSVC 14.44。脚本兼容该宿主并明确指定 v143，保留本地 VS 2022 路径；不把 Windows Server 的 CI 测试当作 Windows 11 实机验收。[官方镜像清单](https://github.com/actions/runner-images/blob/win25-vs2026/20260922.246/images/windows/Windows2025-VS2026-Readme.md)。

## V02 测量工具与实现策略

对应工作区技术选型 7 / 8.1 / V02-1、M01 / M02 最小拖选 / M07 / M09；来源愿景 v000。本轮只做 MAC-01 工具彩排，正式 **30 冷 + 30 热 + 稳定后 5 分钟空闲**在合并后独立执行。工具不会自动将性能代理值标为通过。

| 动作 | macOS | Windows |
| --- | --- | --- |
| NF01 冷 / 热采集 | `bash scripts/measure-nf01.sh --output <新目录>` | `measure-nf01.ps1` 明确失败，待补 |
| NF04 稳定空闲采集 | `bash scripts/measure-idle.sh --output <新目录>` | `measure-idle.ps1` 明确失败，待补 |
| nearest-rank 汇总 | `bash scripts/summarize-nf01.sh --input <JSONL> --output <新目录>` | `summarize-nf01.ps1` 明确失败，待补 |

共享计算脚本 `measure-performance.py` 仅使用 Python 3 标准库；`waibusnap_measure_probe` 为独立外部测量程序，位于 `src/platform/macos/measure_process.mm`，用系统 libproc 读取数据，不链接进应用。没有新增第三方运行时库。CMake 显式链接 AppKit / Carbon / ScreenCaptureKit / CoreGraphics / QuartzCore 系统框架（最低 macOS 14.0）；QuartzCore 仅用于目标屏刷新时钟。

### 运行与非正式彩排

先完成 Release 构建，关闭其他 WaibuSnap 实例。首次从 F1 / 托盘截图完成授权并退出；脚本模式遇到未授权会明确失败，不自动弹授权窗口。工具拒绝输出到已有目录，避免覆盖原始数据。可用 `--build-dir` 指定独立 Release 构建树。

```bash
bash scripts/build-project.sh
bash scripts/test-project.sh
bash scripts/measure-nf01.sh --rehearsal --cold 2 --warm 5 --output build/rehearsal/nf01
bash scripts/measure-idle.sh --rehearsal --duration-seconds 60 --output build/rehearsal/idle
```

正式采集保留默认 30 / 30 和 300 秒，**本轮不执行**。正式模式拒绝源码脏状态、构建脏状态、构建 commit 与当前 commit 不同、非 Release 或少于规定样本数。合并后重新构建，不沿用功能分支二进制。固定等待默认 15 秒，可用 `--stable-seconds` 增加；稳定判据为末 5 次 RSS 波动 ≤ 5 MiB、两半 CPU 均值差 ≤ 0.5 个百分点，至少等待 10 秒。未稳定时退出失败，不选择更低占用片段。

NF01 每个冷样本独立启动新进程，第一次截图标记冷；热采集另启一个进程，首次冷截图留作诊断，后续才进入热汇总。外部工具在预定首次触发前读取稳定基线，并预留 1 秒避免探针与捕获重叠。后续间隔默认 2 秒。测试模式自动取消覆盖层，选区尺寸为 0；取消前已经记录可交互代理终点。

真实 F1 的手动路径（不启用测试模式）：

```bash
bash scripts/run-app.sh --metrics-file "$PWD/build/manual/sessions.jsonl"
# 手动 F1、拖选或 Esc；从托盘退出后汇总。
bash scripts/summarize-nf01.sh --input build/manual/sessions.jsonl --output build/manual/summary
```

无自定义路径时，日志为 Qt `AppLocalDataLocation/WaibuSnap` 对应的 `sessions.jsonl`（实际目录由 Qt / 系统决定）。不保存截图、裁剪图、标注或识别文本；JSONL 仅存时间、尺寸、冷 / 热、触发方式与数值结果码。应用日志的中文错误说明是固定技术提示，不含截图或用户文本。真实 F1 与注入触发直接进入同一个 `ApplicationController::trigger`，第一条语句记录 t0；单会话锁拒绝连按，不为无效重入写虚假成功记录。

### 埋点与呈现误差

每次会话结束写一行 JSONL，纳秒时间戳使用十进制字符串避免 JSON 浮点丢失精度：

- `t0_ns`：热键 / 托盘 / 受控触发进入共享处理函数。
- `capture_start_ns` / `capture_complete_ns`：含可共享内容查询、异步单帧、sRGB 像素转换与 GUI 线程交付。
- `window_created_ns`：已创建覆盖 QWidget 及原生窗口；`paint_complete_ns`：首帧绘制完成，尚不能等同物理呈现。
- `visible_proxy_ns`：Qt backing store 提交后的事件循环，在目标 NSScreen 的第二次 CADisplayLink 回调记录刷新代理；`interactive_ns`：窗口可见且已激活，可接受拖选 / Esc。
- `refresh_interval_ms`：该次显示时钟周期；各段毫秒耗时与源帧 / 选区像素宽高同时保存。

刷新回调只能证明目标屏刷新时钟经过，不能独立证明该窗口已进入物理扫描输出。工具明确使用 `nf01_proxy_ms`，`presentation_validated=false`、`nf01_pass=null`；**正式 NF01 还必须配合平台呈现跟踪或外部高帧率摄像，校验冻结画面与可交互选区真实呈现**。不能用 paintEvent / 均值 FPS 代替该终点，也不能将刷新周期简单减掉宣告通过。GPU 合成延迟与系统负载仍待验证。

有效时间样本必须有可交互终点且结果码属于 `(1, 2, 10)`（历史确认 / 取消 / 已复制并结束会话）；旧数据仍兼容。权限、捕获、显示器变化、呈现观察失败和超时记录均不混入 P95。nearest-rank 为排序后第 `ceil(0.95 × n)` 项，30 次取第 29 项，同时保留最大值。

数值结果码：1 确认（历史语义保留，本轮松开仅进入选区态）、2 取消、3 未授权、4 显示器变化、5 定位未实现 / 失败、6 超时、7 捕获失败、8 未观察到可交互窗口、9 退出取消、10 已复制并结束会话。保存成功不结束会话，不单独写终态日志；受控模式可交互后仍自动取消（码 2），30 冷 + 30 热协议不变。无可交互终点的行不算有效性能样本。

### NF04 进程与内存口径

空闲脚本普通启动应用，**没有 `--test-mode`**；预稳定后采样默认 300 秒，目标间隔 0.8 秒（1.25 Hz），记录实际间隔及探针耗时。不得手动触发截图或操作菜单；若发生会话，数据需作废后重采。

每次探针重新枚举根应用及子孙进程，以 PID + 创建时间区分复用，记录逐进程 RSS、physical footprint 与累计用户态 + 内核态 CPU 时间。RSS 同步总和用于 NF04 的 150 MiB 口径，footprint 另列，不能替换 RSS；共享页可能重复计入。libproc CPU 使用 Mach absolute 单位，按 `mach_timebase_info` 转为纳秒；每轮采集先以 200ms 忙循环对照 `getrusage` 微秒 CPU 时间校准（误差 ≤ 10%）。实现依据为 [Apple XNU task_power_info](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c) 与 [fill_task_rusage](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/bsd_kern.c)。CPU 是全部应用进程时间增量 / 墙钟 × 100%，单逻辑核 = 100%。进程消失 / 读取失败均标缺口，不能把缺失数据当作 0。外部探针不计入应用；它的干扰和采样耗时另记。

对系统共享捕获服务 replayd 另记时序，归属本应用的 RSS 下界为 0、上界为全部候选服务 RSS；与应用 RSS 合计给出保守上界。无法完整读取时上界记为未知，不用下界宣布达标。WindowServer RSS 变化和 CPU 时间增量单列，不全部计入应用。libproc 不允许读取时可用 ps 的 RSS / 10ms CPU 精度旁证并保留权限错误；无法读取者明确不可用。采样前尝试 `vmmap -summary` 保存应用和 WindowServer 的 IOSurface / 图形映射记账；权限不足如实记录，不为测量申请辅助功能或输入监控。WindowServer 是共享服务，无法将整机波动完全归因本应用。采样最大值仅是观察下限，短峰值和短寿命进程可能漏掉；正式报告必须审查归属、采样误差与图形内存缺口。

工具自动保存当前 commit、构建 commit / 脏状态、Release、编译器、SDK / Qt、macOS 小版本 / build、CPU / 内存、电源源与低电量模式、逐屏布局 / backing / 模式像素 / 刷新周期 / EDR、当前输入法 ID、Python / 探针 / 工具版本和工具哈希。不收集序列号、窗口标题、图像或用户文本。显示器 / 输入法 / 电源设置发生变化时重新采集。

### 资源策略与冷热路径

空闲只保留热键注册、托盘菜单、Qt 事件循环与显示器通知连接；无 SCStream、无周期抓屏 / 窗口枚举 / 刷新计时器，无预建选区或隐藏主窗口。Qt 托盘菜单属于必要驻留入口。显示器元数据按触发读取，变更通知取消进行中的截图。

每次有效触发都重新查询 `SCShareableContent`，只捕获鼠标所在显示器的一帧；空排除列表保留可见遮挡，不恢复隐藏窗口内容。`SCScreenshotManager` 内部捕获生命周期由系统管理，应用没有常驻 `SCStream`。SDR / sRGB 通过捕获配置及 CGContext 绘制转换；HDR、P3 精度完整验收仍待验证。

覆盖层每次新建，不重用；QImage 的源像素尺寸与目标屏 DPR 必须匹配，原像素绘制关闭平滑缩放，选区从本屏逻辑坐标转换为半开物理像素矩形。松开进入选区态，移动 / 调整不终结会话；保存成功继续保留；复制成功 / Esc / 取消 / 异常关闭后释放呈现观察器、覆盖层、子工具栏与冻结帧；延迟删除避开正在执行的事件。捕获 / 呈现保护计时器仅会话活跃时启用，可交互后停止。反馈计时器仅成功保存后单次清除提示，随会话销毁；注入测试定时器仅显式受控模式启用。

冷 / 热路径使用同一策略，差异来自进程首次运行时 Qt / 字体 / OS 捕获服务的自然初始化与缓存；没有应用层截图预热、旧帧复用或持久捕获缓冲。工具终止空闲测试进程只是采集清理，不执行工作集 trim 或强制 GC；产品正常退出走托盘并释放热键。

### 尚待验证

MAC-01 单屏工具彩排不能替代 30 / 30、5 分钟空闲、Intel 实机、macOS 14 下限、Windows 11、负坐标 / 混合 DPI 外屏、热插拔、运行中撤权、F1 占键冲突、无 AX / 输入监控的干净账户或长时间驻留。Fn 是否向应用发送裸 F1 受系统设置影响；原型不提供改键设置。正式采集与工作区验证记录在合并后另行登记。
