# WaibuSnap

跨平台 PC 截图工具，目标平台为 macOS 14.0+（arm64 / x86_64）和 Windows 11 x64。

当前是**工程初始化骨架**：Qt 6.11.2 Widgets / C++17 空窗口，可启动、关闭；没有截图、热键、贴图或其他 Must 功能。macOS 使用 Objective-C++ 平台编译入口，Windows 使用 MSVC 平台入口。

## 环境准备

环境脚本只检测并给出指引，不自动下载或安装。Qt 尚未安装时，由开发者自行决定安装时间。

- macOS：Xcode 15+ / 对应命令行工具、CMake 3.24+、Ninja、Qt **6.11.2** macOS 桌面组件。
- Windows：PowerShell **7+**（`pwsh`）、Visual Studio **2022** 的“使用 C++ 的桌面开发”（含 MSVC x64 和 Windows SDK）、CMake 3.24+、Qt **6.11.2 MSVC 2022 64-bit**。不使用 MinGW 版 Qt。
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

默认构建当前主机架构的 Release，产物为 `build/macos/bin/WaibuSnap.app`。运行脚本直接启动包内程序；关闭窗口即退出。测试由 CTest 设置 `QT_QPA_PLATFORM=offscreen`，不需要截图权限或可见桌面。

Intel 交叉构建路径（**待验证**，使用独立构建目录）：

```bash
MACOS_ARCHITECTURES=x86_64 WAIBUSNAP_BUILD_DIR="$PWD/build/macos-x86_64" bash scripts/build-project.sh
```

是否可构建取决于所装 Qt kit 的 x86_64 slice；Apple Silicon 上的交叉构建或 Rosetta 运行不能代替 Intel 实机验收。

## Windows：构建、运行与测试

在 PowerShell 7 的源码仓库目录中执行，Qt 路径按实际安装位置填写：

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.2/msvc2022_64'
./scripts/prepare-environment.ps1
./scripts/build-project.ps1
./scripts/test-project.ps1
./scripts/run-app.ps1
```

默认构建 MSVC x64 Release，产物为 `build/windows-x64/bin/Release/WaibuSnap.exe`。测试和运行脚本会把所选 Qt 的 `bin` 加入当前进程 PATH，使 DLL 可被加载；无需将 Qt 加入系统级 PATH。

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
