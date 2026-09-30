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

无自动下载、安装、打包或发布脚本。失败返回非零退出码；测试使用 `--no-tests=error`，避免零测试假通过。

## 手动准备与版本锁定

Qt 通过 [官方开源安装器](https://www.qt.io/download-qt-installer-oss)选择 **6.11.2**，只需桌面基础组件 qtbase（Core / Gui / Widgets / Test 及必需插件），不用 QML / WebEngine 或 GPL-only 模块。安装器可能同时展示其他组件，勿默认勾选。将 `QT_ROOT_DIR` 指向 kit 根目录。

- macOS：准备 Xcode 15+ / 命令行工具；CMake 3.24+ 与 Ninja 可由开发者自行通过官方安装包或 Homebrew 安装（例如 `brew install cmake ninja`，脚本不会执行该命令）。
- Windows：准备 Visual Studio 2022 C++ 桌面开发、Windows SDK、[CMake](https://cmake.org/download/)和 [PowerShell 7](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)。CMake 与 pwsh 加入当前用户 PATH。优先使用 VS 2022；兼容 VS 2026 宿主加装 MSVC v143 14.44 x64（需要 CMake ≥ 4.2）。脚本按已安装宿主选生成器并固定 `-T v143`，无需另装 Ninja 或手动运行 vcvars。
- 可选的本地格式工具：已有 Python 3 时，可自行创建 `.venv`，在其中运行 `python -m pip install clang-format==18.1.8`，激活后执行 lint 脚本。只在需要格式检查时安装，不是应用构建的前置条件。

环境脚本用 qmake 检查 **6.11.2**，Windows 还检查 MSVC kit；CMake `EXACT` 再次检查版本并拒绝静态 Qt。CI action、Qt、aqtinstall、py7zr 与 clang-format 均有固定版本或提交；runner 镜像和系统 SDK 随 `*-latest` 更新，详细版本以每次 CI 日志为准，不声称完整可复现构建。

## 参数

| 环境变量 | 默认 / 作用 |
| --- | --- |
| `QT_ROOT_DIR` | Qt kit 根目录；未指定时从 PATH qmake 检测 |
| `WAIBUSNAP_BUILD_DIR` | macOS 为仓库内 `build/macos`，Windows 为 `build/windows-x64`；建议自定义为绝对路径 |
| `WAIBUSNAP_BUILD_TYPE` | `Release`；也接受 Debug / RelWithDebInfo / MinSizeRel；同一轮构建、测试、运行须保持一致 |
| `MACOS_ARCHITECTURES` | 仅 macOS，默认 `uname -m`；可设 x86_64 或 `arm64;x86_64`（x86_64 已通过 CI 交叉编译，通用二进制与 Intel 实机待验证） |

macOS 配置固定部署下限 14.0，Windows 固定 MSVC v143 x64（宿主可为 VS 2022 / 2026）。脚本不申请管理员权限、不变更系统策略。自定义架构请使用不同构建目录。

脚本始终开启 `BUILD_TESTING`。需要仅构建应用时，可手动使用相同 CMake 参数并传 `-DBUILD_TESTING=OFF`；此时既不查找 Qt Test，也不编译 `--smoke-test` 自动关闭入口。没有发布包入口，不能将该构建开关等同于发布验收。

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
