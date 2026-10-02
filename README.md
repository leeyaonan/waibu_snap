# WaibuSnap

跨平台 PC 截图工具，目标平台为 macOS 14.0+（arm64 / x86_64）和 Windows 11 x64。

当前进入**功能开发：选区交互与输出闭环**。macOS 托盘驻留、Carbon F1、鼠标所在显示器的 ScreenCaptureKit 单帧捕获；冻结画面可框选、移动、拖动四边 / 四角调整大小，实时显示物理像素尺寸；浮动工具栏支持复制、保存 PNG 与取消。仍无标注、贴图、窗口吸附、放大镜、键盘像素微调、设置界面或 JPEG。Windows 同步编译共享几何、视图与输出代码，但原生热键、显示器定位及捕获如实返回“未实现”。性能门槛与完整平台兼容性仍按实机记录验收。

运行后使用菜单栏“截图”或 F1（Fn 模式由系统决定）；首次截图需要屏幕录制授权。拒绝时显示中文说明，不生成伪成功画面；授权后重试，系统要求时重启。此原型不申请辅助功能或输入监控。

拖选松开后保留选区，内部拖动保持尺寸，边角调整夹取在起始屏内。反向拖过对侧时夹停在对侧前 1 个物理像素，不翻转手柄；选区外单击清除，选区外拖动替换。零面积不进入选区态。复制成功结束会话，失败保留并可点「复制」重试；只有复制入口写剪贴板。保存成功短暂显示文件名且保留会话，继续移动 / 调整即重新标记未保存；取消、关闭或 Esc 均不产生额外输出，主动保存的文件保留。本轮不做未保存退出提示。

保存使用原生 PNG 面板和带时间戳的建议文件名；缺少 `.png` 时自动追加（其他后缀也追加 `.png`，不会输出 JPEG）。覆盖确认交给原生面板，补后缀后碰到已有文件会重新打开面板确认最终路径。保存取消返回选区；失败显示中文原因并可重新选择路径，原子写入失败保留原文件。面板期间 Esc 由面板处理，关闭面板后恢复覆盖层焦点。

复制和保存都只从冻结帧按当前物理像素选区裁剪，输出副本显式设置 DPR=1；遮罩、尺寸提示、边角手柄和工具栏仅在 QWidget 显示层绘制，裁剪不读取该显示层，因此不会进入输出。除显式保存外不生成用户图片文件；会话日志沿用时间、尺寸与数值结果码，不增加图像、文件名或输出路径。

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
bash scripts/run-app.sh
```

默认构建当前主机架构的 Release，产物为 `build/macos/bin/WaibuSnap.app`。运行脚本直接启动包内程序；应用以托盘驻留，关闭选区不会退出，退出走托盘菜单。测量彩排、真实 F1 日志与完整口径见 [脚本登记](scripts/README.md#v02-测量工具与实现策略)。测试由 CTest 设置 `QT_QPA_PLATFORM=offscreen`，不需要截图权限或可见桌面。

Intel 交叉构建路径（CI 已通过编译，**Intel 实机运行待验证**，使用独立构建目录）：

```bash
MACOS_ARCHITECTURES=x86_64 WAIBUSNAP_BUILD_DIR="$PWD/build/macos-x86_64" bash scripts/build-project.sh
```

CI 使用 Qt kit 的 x86_64 slice 交叉编译成功；Apple Silicon 上的交叉构建或 Rosetta 运行不能代替 Intel 实机验收。

## Windows：构建、运行与测试

在 PowerShell 7 的源码仓库目录中执行，Qt 路径按实际安装位置填写：

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.2/msvc2022_64'
./scripts/prepare-environment.ps1
./scripts/build-project.ps1
./scripts/test-project.ps1
./scripts/run-app.ps1
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

本轮没有安装包、签名、公证或发布入口。构建树 `.app` / `.exe` 仍依赖开发机 Qt，不可直接作为完整预览包分发。开发 bundle id 默认为 `local.waibusnap.dev`，可用 CMake `-DWAIBUSNAP_BUNDLE_ID=...` 覆盖；正式标识待确认。

自有代码采用 [MIT](LICENSE)。Qt Core / Gui / Widgets 采用 LGPLv3 动态链接；Qt Test 仅用于测试，不链接到应用。自有静态模块不会把 Qt 静态链接进程序。后续发行须补齐 Qt 及传递依赖许可、对应源码、替换 / 重链接与必要重签说明；本次尚未完成发行合规验收。
