# WaibuSnap

跨平台 PC 截图工具，目标平台为 macOS 14.0+（arm64 / x86_64）和 Windows 11 x64。

当前进入**功能开发：贴图内编辑与未保存确认（第四刀 4b）**。macOS 托盘驻留、Carbon 可改全局截图键、鼠标所在显示器的 ScreenCaptureKit 单帧捕获；冻结画面可悬停预览普通窗口、单击吸附或自由框选，随后移动、拖动四边 / 四角调整大小，实时显示物理像素尺寸；浮动工具栏提供矩形、椭圆、直线、箭头、自由画笔、文本，支持颜色 / 线宽 / 字号预设、最近 20 步撤销 / 重做、复制、保存 PNG、钉到屏幕与取消。托盘提供「截图 / 设置… / 退出」。支持多张无边框置顶贴图、拖动、25%–400% 缩放与原像素保存 / 复制，贴图可进入六工具编辑、保存后再改标记未保存，关闭 / 退出支持保存、取消或放弃。MVP Must 共享功能面已收口，剩余为验收类工作及已有 Windows 原生适配缺口；仍无马赛克 / 实心遮盖、标注对象再编辑、放大镜、键盘像素微调或 JPEG。Windows 同步编译共享几何、标注、设置、视图与合成输出代码，原生热键、显示器定位、捕获与窗口枚举如实返回“未实现”。性能门槛与完整平台兼容性仍按实机记录验收。

运行后使用菜单栏“截图”或 F1（Fn 模式由系统决定）；首次截图需要屏幕录制授权。拒绝时显示中文说明，不生成伪成功画面；授权后重试，系统要求时重启。此原型不申请辅助功能或输入监控。

已接入应用图标（访达 / 信息窗可见；Dock 不显示），Windows 可执行文件同步嵌入多尺寸图标；菜单栏 / 托盘图标已替换为歪布简化版「猫头＋取景框」，macOS 使用黑色模板，Windows 使用彩色版。主图与再生成命令见 [图标资源说明](assets/icons/README.md)；Windows 11 图标显示待实机核对。

空闲选区态悬停高亮光标下最前的普通窗口，单击仅确认选区，不直接复制或保存。窗口外框包含标题栏，越屏部分裁剪到会话所在屏；输出仍为冻结桌面中当时可见的像素，保留其他窗口遮挡。窗口列表在采集成功后、创建覆盖层前读取一次，会话内不扫描；枚举失败或没有可吸附窗口时，提示手动框选。吸附后可移动、调整，外部单击清除后恢复悬停，外部拖动可替换选区。

「设置…」支持 F1–F12 或带 Ctrl / Command、Control、Alt / Option 的字母与数字；拒绝空键、多段序列、裸字母 / 数字、仅 Shift 的字母 / 数字以及空格、Tab、Esc 等未支持键。新键注册成功后释放旧键并保存，失败保持旧绑定与配置；注册成功但设置写入失败时明确提示本次已生效、重启后可能无法保留，允许重试。Mac 的 ⌘⇧3 / 4 / 5 提示可能被系统占用并拒绝绑定，不抢占系统截图组合。

设置使用 `AppLocalDataLocation/settings.ini`，唯一键 `hotkey/sequence` 按 PortableText 存储，界面按 NativeText 展示。启动时非法配置明确回退 F1；合法存储键注册失败则尝试 F1，两者均失败时仅托盘入口可用，tooltip 反映实际状态。启动回退不自动改写文件。设置打开期间暂停截图热键，截图会话期间「设置…」置灰，关闭会话后恢复；取消或 Esc 不保存，退出释放热键。

开发与受控验证使用 `--settings-file <临时 INI 路径>` 隔离配置，此参数不要求 `--test-mode`；省略时才使用正常配置域。原有受控参数与测量日志格式保持不变；新增钉图成功结果码 11，既有结果码语义不变，受控注入仍自动取消码 2。

macOS 普通运行在 `AppLocalDataLocation/waibusnap.lock` 持有单实例锁，重复启动打印中文说明并正常退出，避免重复托盘和热键冲突。`--test-mode` / `--smoke-test` 跳过该锁；`--settings-file` 仅隔离设置，不改变锁的位置或单实例规则。无法创建应用数据目录时启动失败。平台策略暂不在 Windows 启用该锁，保持 Windows 原有启动行为。

拖选松开后保留选区，内部拖动保持尺寸，边角调整夹取在起始屏内。反向拖过对侧时夹停在对侧前 1 个物理像素，不翻转手柄；选区外单击清除，选区外拖动替换。零面积不进入选区态。复制成功结束会话，失败保留并可点「复制」重试；只有复制入口写剪贴板。保存成功短暂显示文件名且保留会话，继续移动 / 调整选区、增加标注、撤销 / 重做即重新标记未保存；取消、关闭或 Esc 均不产生额外输出，主动保存的文件保留。选区会话退出仍以码 9 取消，不增加确认；贴图确认见下文。

保存使用原生 PNG 面板和带时间戳的建议文件名；缺少 `.png` 时自动追加（其他后缀也追加 `.png`，不会输出 JPEG）。覆盖确认交给原生面板，补后缀后碰到已有文件会重新打开面板确认最终路径。保存取消返回选区；失败显示中文原因并可重新选择路径，原子写入失败保留原文件。面板期间 Esc 由面板处理，关闭面板后恢复覆盖层焦点。

选区存在时，点击工具启用标注，任意画布位置的拖动优先绘图；再次点击当前工具恢复选区移动 / 调整。颜色为红 / 黄 / 蓝，图形线宽为 2 / 4 / 8 个物理像素，文本字号为 18 / 28 / 40 个物理像素。标注使用冻结帧的绝对物理像素坐标，选区移动 / 调整只改变裁剪窗口；超出选区的部分在导出时裁剪。撤销 / 重做提供按钮与系统标准快捷键（macOS ⌘Z / ⇧⌘Z，Windows Ctrl+Z / Ctrl+Y），仅作用于当前图片的编辑态；撤销后新增会清除重做分叉，早于最近 20 步的标注仍保留在画面中。复制成功、取消或关闭后丢弃标注与编辑历史，不写文件或恢复会话。

文本工具单击落点打开内嵌多行编辑器，支持中文输入与换行；点击其他位置、切换工具或 ⌘ / Ctrl+Enter 提交为一条标注，空文本不添加。复制 / 保存 / 钉图会先提交正在编辑的文本。输入法预编辑期间按键由编辑器处理；Esc 先退出候选，再取消当前文本编辑，编辑器关闭后再次 Esc 才取消会话。实际搜狗 / 系统输入法候选操作、缺字与中文清晰度仍须按 [人工核对清单](tests/README.md#人工核对清单待用户真机操作) 验证。

复制和保存统一通过 `renderAnnotatedSelection`，在冻结帧物理像素中合成标注并裁剪到当前选区，输出副本 DPR=1。预览与导出共用 `paintAnnotation` / `paintAnnotations`；遮罩、尺寸提示、边角手柄、悬停高亮、工具栏和文本输入框仅属于 QWidget 显示层，合成不读取显示层。除显式保存外不生成用户图片文件；会话日志沿用时间、尺寸与数值结果码，不增加图像、标注文本、文件名或输出路径。

点击「钉到屏幕」会提交当前文本并合成冻结帧与标注，在选区全局左上角创建贴图；成功以结果码 11 结束会话并清空编辑历史，失败保留会话并允许重试。钉图不写剪贴板、不自动保存文件。每张贴图独立持有钉图原图与保存状态；鼠标进入显示「－ / ＋ / 100% / 保存 / 复制 / 编辑 / ×」，离开隐藏，极小贴图折叠为「⋯」菜单，也可右键访问全部操作。滚轮以光标为锚点缩放，按钮以窗口中心为锚点；跨屏拖动按目标屏 DPR 调整逻辑尺寸，100% 按原物理像素清晰绘制。Qt 顶层窗口使用整数逻辑尺寸，非整除尺寸向上取整（最小 1），避免裁切原像素。

贴图缩放不改变保存 / 复制内容及像素尺寸；保存沿用原生 PNG 面板、覆盖确认与原子提交，成功保留贴图并标记已保存，复制成功只更新剪贴板。移除屏幕或改变布局后，屏外贴图移回主屏可见区域。关闭最后一张贴图仍保持托盘与截图键；退出销毁全部贴图、选区并释放热键。已保存贴图直接关闭；未保存贴图弹「取消 / 保存 / 放弃」，保存面板取消或失败保留贴图并可重试。托盘退出先提交贴图中的未提交文本，再对未保存贴图汇总提示「取消退出 / 逐张保存 / 全部放弃」；逐张按创建顺序保存，任何取消或失败中止退出，已保存的保持已保存。确认与保存面板期间再次退出会被中止，清理路径强制关闭而不重复弹窗。没有隐藏 / 恢复全部贴图、剪贴板贴图、旋转、透明度、穿透或历史恢复。

贴图可通过「编辑」、右键菜单或双击图像进入编辑，六类工具与覆盖层共用标注模型、20 步历史、文本编辑器和渲染函数。编辑态拖动只绘图，无工具时不移动窗口；滚轮仍以光标为锚点缩放。「完成」或 Esc 退出编辑并保留标注，再拖动恢复移动。标注坐标为基图物理像素，显示按缩放 ÷ 当前屏 DPR 变换；复制 / 保存始终按原尺寸合成、DPR=1，不含控件。钉图时烘焙的标注已属于基图，不支持独立选择、移动或删除。文本提交与 Esc 分层沿用覆盖层；进入编辑临时允许键盘输入，退出恢复不抢前台应用键盘的面板模式，焦点取舍见 [源码说明](src/README.md)。

## 环境准备

环境脚本只检测并给出指引，不自动下载或安装。Qt 尚未安装时，由开发者自行决定安装时间。

- macOS：Xcode 15+ / 对应命令行工具、CMake 3.24+、Ninja、Qt **6.11.2** macOS 桌面组件。
- Windows：PowerShell **7+**（`pwsh`）、Visual Studio **2022** 的“使用 C++ 的桌面开发”（含 MSVC x64 和 Windows SDK）、CMake 3.24+、Qt **6.11.2 MSVC 2022 64-bit**。也可使用 VS 2026 宿主加装 **MSVC v143 14.44 x64**，此时 CMake 必须 ≥ 4.2。不使用 MinGW 版 Qt。
- Qt 可通过 [Qt 开源下载入口](https://www.qt.io/download-qt-installer-oss)安装；组件选择固定 **6.11.2**，macOS 为 `macos`，Windows 为 `msvc2022_64`。不使用跟随最新版的包管理命令代替版本锁定。
- `QT_ROOT_DIR` 指向具体 kit 根目录（其下有 `bin/qmake` 或 `bin/qmake.exe`）。可省略该变量并从 PATH 中的 qmake 查找；版本不匹配会报错。

完整脚本参数、工具安装指引和依赖 / 许可证表见 [scripts/README.md](scripts/README.md)。CMake 同时使用 `find_package(Qt6 6.11.2 EXACT ...)` 和动态库类型检查，拒绝其他版本或静态 Qt。

## macOS：构建、运行与测试

以下在源码仓库根目录执行，Qt 路径按实际安装位置填写：

```bash
export QT_ROOT_DIR="$HOME/Qt/6.11.2/macos"
bash scripts/prepare-environment.sh
bash scripts/build-project.sh
bash scripts/test-project.sh
settings_dir="$(mktemp -d)"
bash scripts/run-app.sh --settings-file "$settings_dir/settings.ini" --metrics-file "$settings_dir/sessions.jsonl"
```

默认构建当前主机架构的 Release，产物为 `build/macos/bin/WaibuSnap.app`。运行脚本经 LaunchServices（`open`）启动，参数通过 `--args` 透传，stdout / stderr 分别保存到 `build/macos/logs/run-app.stdout.log` 与 `run-app.stderr.log`，终端跟随 stderr 并等待应用退出；Ctrl+C 只结束脚本，应用继续留在托盘。已有实例时脚本提示先退出或直接使用它。应用以托盘驻留，关闭选区不会退出，退出走托盘菜单。测量彩排、真实 F1 日志与完整口径见 [脚本登记](scripts/README.md#v02-测量工具与实现策略)。无头测试不需要截图权限或可见桌面。

Intel 交叉构建路径（CI 已通过编译，**Intel 实机运行待验证**，使用独立构建目录）：

```bash
MACOS_ARCHITECTURES=x86_64 WAIBUSNAP_BUILD_DIR="$PWD/build/macos-x86_64" bash scripts/build-project.sh
```

CI 使用 Qt kit 的 x86_64 slice 交叉编译成功；Apple Silicon 上的交叉构建或 Rosetta 运行不能代替 Intel 实机验收。

## macOS 27 已知问题与兼容处理

Qt 6.11.2 的 `QSystemTrayIcon` 假定 `NSApp.currentEvent` 是鼠标事件；macOS 27 的手势回调可能携带 KitDefined 事件，读取 `clickCount` 会触发 AppKit 异常（[QTBUG-147449](https://bugreports.qt.io/browse/QTBUG-147449)）。当前在创建窗口 / 托盘前幂等安装本进程 `NSEvent.clickCount` 绕行：鼠标事件调用原实现，其他事件返回 1。它影响本进程该方法的所有调用；合成事件回归测试不能替代托盘反复点击的真机核对。

本轮保持 Qt **6.11.2**。升级到含官方修复的版本（≥6.12.0，或包含 qtbase 6.11 分支 [6192d9edd0](https://github.com/qt/qtbase/commit/6192d9edd00caa14ed6b67c32c0cd8cfe95cf815) 的 6.11.x）后删除绕行、安装入口及相应测试。

macOS 27 起，屏幕录制授权按 TCC 的责任进程归属判定。从终端直接执行包内二进制时，应用已获授权仍可能被判未授权。需要真实截图时，通过访达、`open build/macos/bin/WaibuSnap.app` 或 `bash scripts/run-app.sh` 启动；授权或撤销后按系统要求重启应用。NF01 采集同样使用 LaunchServices，详见 [授权归属说明](scripts/README.md#macos-27-授权归属)。

本机开发已支持固定自签名证书「WaibuSnap Dev」：配好证书并完成首次切换的一次性授权后，同一证书与 bundle id 下重建不再掉屏幕录制授权。未配置证书的机器仍使用 ad-hoc，二进制变化后需移除旧记录并在普通启动中重新授权；测试模式不会弹授权窗，可能静默记录码 3。创建、跳过和校验步骤见 [本机稳定签名](scripts/README.md#本机稳定签名)。CI 继续 ad-hoc，Windows 与发布策略不变。

## Windows：构建、运行与测试

在 PowerShell 7 的源码仓库目录中执行，Qt 路径按实际安装位置填写：

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.2/msvc2022_64'
./scripts/prepare-environment.ps1
./scripts/build-project.ps1
./scripts/test-project.ps1
$settingsDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $settingsDir | Out-Null
./scripts/run-app.ps1 --settings-file (Join-Path $settingsDir 'settings.ini') --metrics-file (Join-Path $settingsDir 'sessions.jsonl')
```

默认构建 MSVC v143 x64 Release，产物为 `build/windows-x64/bin/Release/WaibuSnap.exe`。测试和运行脚本会把所选 Qt 的 `bin` 加入当前进程 PATH，使 DLL 可被加载；无需将 Qt 加入系统级 PATH。

## 格式与 CI

```bash
bash scripts/lint-code.sh
```

```powershell
./scripts/lint-code.ps1
```

双端使用 clang-format **18.1.8**。CI 在 `macos-latest` 与 `windows-latest` 安装精确版本 Qt，调用与本地相同的构建 / 测试 / 格式脚本。测试共用 Qt Test + CTest，包含真实应用进程的启动与退出。检查状态见 [GitHub Actions](https://github.com/leeyaonan/waibu_snap/actions)。CI 不能替代 Windows 11、Intel Mac 或产品功能实机验证。

## 目录与交付边界

共享接口在 `src/interfaces/`；平台实现只在 `src/platform/macos/` 和 `src/platform/windows/`；其余模块边界见 [src/README.md](src/README.md)。产品愿景、设计和规格在本地独立工作区维护。

本轮只有可选的本机开发自签名，没有安装包、分发签名、公证或发布入口。构建树 `.app` / `.exe` 仍依赖开发机 Qt，不可直接作为完整预览包分发。开发 bundle id 默认为 `local.waibusnap.dev`，可用 CMake `-DWAIBUSNAP_BUNDLE_ID=...` 覆盖；变更时须同步构建脚本的签名标识并重新授权屏幕录制，正式标识待确认。

自有代码采用 [MIT](LICENSE)。Qt Core / Gui / Widgets 采用 LGPLv3 动态链接；Qt Test 仅用于测试，不链接到应用。自有静态模块不会把 Qt 静态链接进程序。后续发行须补齐 Qt 及传递依赖许可、对应源码、替换 / 重链接与必要重签说明；本次尚未完成发行合规验收。
