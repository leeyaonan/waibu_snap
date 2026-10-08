# 应用与菜单栏 / 托盘图标资源

最终图标为用户确认的候选 01「正面按快门」暖底主图。这里只做等比例缩放与容器格式转换，主图原样入库，不重新绘制、配色、裁剪或添加平台底衬。

| 文件 | 用途 / 尺寸 |
| --- | --- |
| `waibusnap-1024.png` | 1024×1024 PNG，所有再生成的唯一基准 |
| `waibusnap.icns` | macOS bundle 图标；iconset 含 16 / 32 / 128 / 256 / 512 pt 及全部 @2x 档位，最大 1024 px |
| `waibusnap.ico` | Windows 可执行文件图标；16 / 24 / 32 / 48 / 64 / 128 / 256 px，全部为无损 PNG 载荷（包括 256 px） |
| `waibusnap.rc` | Windows RC 图标声明，文件名相对本目录；仅在 WIN32 构建中加入应用目标 |
| `tray/waibusnap-tray-mask.png` | macOS 菜单栏黑色模板，18×18 px，纯黑 RGB + alpha |
| `tray/waibusnap-tray-mask@2x.png` | 同一确认稿的 Retina 模板，36×36 px，18 pt @2x |
| `tray/waibusnap-tray-color.png` | Windows 托盘彩色版，16×16 px，透明底 |
| `tray/waibusnap-tray-color@2x.png` | 同一确认稿的彩色版，32×32 px，16 pt @2x |

基准主图 SHA-256：

```text
f867192750400ac745bbe6a99c8910c50f02eddb33a4183f25f45d7294b78bf0
```

macOS 的 `CFBundleIconFile` 指向 bundle Resources 内的 `waibusnap.icns`。应用保持 `LSUIElement`：图标用于访达和信息窗，Dock 不显示。Windows 应用图标进入 EXE 的资源段；Windows 11 实机显示仍须核对，CI 的 MSVC 资源编译不能替代实机验收。

## 菜单栏 / 托盘确认稿与再生成

用户于 2026-10-08 明确选择托盘候选 03「猫头＋取景框」。四个规格化 PNG 从该候选的 macOS 18 / 36px、Windows 16 / 32px 导出文件逐字节复制，仅改为工程文件名；本轮没有重绘、重新配色或重采样。App 图标的候选 01 主图、ICNS、ICO 与 RC 保持原样。

确认稿 SHA-256：

```text
f9a99b93a1d4eef6f64464e83fee70aa55950a8e2118813438a1a12fe657fac1  tray/waibusnap-tray-color.png
6c2ecc805dc07ca5210ba84431b9e548d15e8aac13fe94da703579f2a50d54e8  tray/waibusnap-tray-color@2x.png
3ca7ac13d5271fb69f622f25d6de20d60bd25340a04adbf9e453dab1be0b38a5  tray/waibusnap-tray-mask.png
0d80588f86c9849307349c83affc89c0a310d4d364193763c6b430058d92d46c  tray/waibusnap-tray-mask@2x.png
```

这四个 PNG 是托盘资源的入库基准；恢复资源时直接复制对应档位的确认稿并核对上述摘要，不从 App 主图、模拟条或缩略预览重新提取。本轮档位齐全，无需补图。以后个别档位缺失时，仅从同一确认候选的黑 / 彩放大母版按原比例导出相应方形 PNG，保存 alpha；不描摹、不重新量化颜色、不串行缩放，也不使用 AI 重新生成。现有齐全档位不覆盖。

`src/platform/CMakeLists.txt` 使用 `qt_add_resources` 将四个 PNG 嵌入平台库，资源路径为 `:/icons/tray/<文件名>`，静态库的资源对象随链接进入应用与测试，不依赖工作目录或开发机路径。macOS 的 `createTrayIcon()` 返回黑版并设置 `QIcon::setIsMask(true)`；Windows 返回彩色版、保持非模板。两端均显式加入 @2x 文件，使初始屏幕为 1x 时也保留完整档位。机制依据 [Qt 的资源嵌入](https://doc.qt.io/qt-6/qt-add-resources.html) 与 [Qt 6.11.2 的 QIcon 高 DPI 加载](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/image/qicon.cpp)。

自动测试验证资源能解码、平台尺寸、模板标记与 1x / 2x 输出的像素尺寸和 DPR；实际菜单栏浅色 / 深色 / 打开菜单高亮、Retina 清晰度与 Windows 11 托盘显示按 [人工核对清单](../../tests/README.md#人工核对清单待用户真机操作) 验收。

## 再生成（一次性 macOS 本地工具）

在源码仓库根目录执行以下命令。只使用系统 `sips` / `iconutil` 和 Python 3 标准库；没有 Pillow 或其他额外包，不需要虚拟环境，也不向应用构建、运行时或 CI 引入图像转换依赖。Windows 开发直接使用已提交的 ICO / RC；ICNS 由 macOS 的 `iconutil` 生成。

每个尺寸直接从 1024 主图缩放，避免逐级缩放损失；画布始终为方形，保持原比例、原背景与完整内容。临时 iconset / PNG 随命令结束自动清理。

```bash
python3 - assets/icons <<'PY'
"""一次性 macOS 本地转换工具，不进入应用构建依赖。"""
import pathlib
import struct
import subprocess
import sys
import tempfile

icons = pathlib.Path(sys.argv[1]).resolve()
source = icons / "waibusnap-1024.png"
with tempfile.TemporaryDirectory(prefix="waibusnap-icons-") as temporary:
    root = pathlib.Path(temporary)
    iconset = root / "waibusnap.iconset"
    iconset.mkdir()
    for base in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            size = base * scale
            suffix = "@2x" if scale == 2 else ""
            target = iconset / f"icon_{base}x{base}{suffix}.png"
            subprocess.run(
                ["sips", "-z", str(size), str(size), str(source), "--out", str(target)],
                check=True,
                stdout=subprocess.DEVNULL,
            )
    subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(icons / "waibusnap.icns")],
        check=True,
    )
    sizes = (16, 24, 32, 48, 64, 128, 256)
    payloads = []
    for size in sizes:
        target = root / f"windows-{size}.png"
        subprocess.run(
            ["sips", "-z", str(size), str(size), str(source), "--out", str(target)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        payloads.append(target.read_bytes())
    directory = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for size, payload in zip(sizes, payloads):
        # PNG 的 IHDR 指示实际通道深度；确认版是 RGB，编码为 24 位无损 PNG。
        bit_depth, color_type = payload[24], payload[25]
        channels = {2: 3, 6: 4}[color_type]
        bits = bit_depth * channels
        dimension = 0 if size == 256 else size
        directory.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, bits, len(payload), offset))
        offset += len(payload)
    (icons / "waibusnap.ico").write_bytes(bytes(directory) + b"".join(payloads))
print("ICNS 与 ICO 已由同一确认主图生成。")
PY
```

## 接入与核对

- macOS：`src/CMakeLists.txt` 设置 `MACOSX_BUNDLE_ICON_FILE`，ICNS 通过 `MACOSX_PACKAGE_LOCATION=Resources` 随目标复制；自定义 plist 引用相同变量。构建脚本继续使用既有稳定开发签名。
- Windows：仅在 `WIN32` 启用 RC 并加入 `waibusnap.rc`；本目录作为资源编译搜索路径，使相对 ICO 文件名可在构建目录中解析。
- 重新生成后，核对主图 SHA-256、ICNS 往返 iconset 档位、ICO 的 7 个目录项及 PNG 像素尺寸，再运行构建、CTest 与 clang-format 18.1.8。
- Finder 若仍显示旧图标，先在独立临时位置复制一份已构建的 .app 再核对；移动构建产物或重启访达由使用者按需操作，不终止运行中的 WaibuSnap 实例。

macOS 接入使用 [CMake 的 plist 配置约定](https://cmake.org/cmake/help/latest/prop_tgt/MACOSX_BUNDLE_INFO_PLIST.html)；Windows RC 使用 [Microsoft 的 ICON 资源语法](https://learn.microsoft.com/en-us/windows/win32/menurc/icon-resource)。

```bash
app=build/macos/bin/WaibuSnap.app
test -f "$app/Contents/Resources/waibusnap.icns"
plutil -p "$app/Contents/Info.plist"
codesign --verify --verbose=2 "$app"
codesign -d -r- "$app"
```

## 本机接入验证（2026-10-08）

- macOS / arm64、Qt 6.11.2 Release 构建、11 项 CTest、clang-format 18.1.8 通过。
- bundle Resources 内的 ICNS 与入库文件逐字节一致，plist 的 `CFBundleIconFile` 为 `waibusnap.icns`，`LSUIElement` 保持 true。签名验证通过，仍为 WaibuSnap Dev 与原 bundle id / 证书叶 requirement。
- ICNS 往返展开具备全部十个 iconset 文件，1024 档解码像素与确认主图完全一致；ICO 七个目录项均含与目录尺寸一致的 PNG 载荷。
- 访达「WaibuSnap.app 简介」的顶部图标与展开预览已通过界面目测，显示确认的 01 构图；未移动应用或重启访达。
- 通过 `open -W -n` 启动独立受控进程，临时 INI / JSONL 隔离；两条注入均码 2、冷 / 热顺序正确、冻结帧 3024×1964、选区尺寸 0、可见代理与交互终点非零。原有普通实例保持运行；未修改屏幕录制授权或测量协议。首轮鼠标所在外显捕获为 1920×1080，两条同样码 2；内置屏补验满足本轮指定帧尺寸。
- Windows 11 实机图标显示仍待核对；双端 CI 的最终状态见本次 PR。原始受控日志与一次性工具仅保留本机临时目录，不入库。
