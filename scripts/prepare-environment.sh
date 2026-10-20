#!/usr/bin/env bash
# 仅检测环境并提供安装指引，不下载或安装软件。
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo "本脚本只支持 macOS。" >&2; exit 1; }
for tool in cmake ctest ninja; do
    command -v "$tool" >/dev/null || { echo "缺少 ${tool}；请按 scripts/README.md 手动准备环境。" >&2; exit 1; }
done
xcrun --find clang++ >/dev/null
if [[ -z "${QT_ROOT_DIR:-}" ]]; then
    qt_qmake="$(command -v qmake6 || command -v qmake || true)"
    [[ -n "$qt_qmake" ]] || { echo "未找到 Qt；请安装 Qt 6.11.2 macOS 桌面组件并设置 QT_ROOT_DIR。" >&2; exit 1; }
    QT_ROOT_DIR="$("$qt_qmake" -query QT_INSTALL_PREFIX)"
fi
qt_qmake="$QT_ROOT_DIR/bin/qmake"
[[ -x "$qt_qmake" ]] || { echo "QT_ROOT_DIR 必须指向包含 bin/qmake 的 Qt 目录。" >&2; exit 1; }
qt_version="$("$qt_qmake" -query QT_VERSION)"
[[ "$qt_version" == 6.11.2 ]] || { echo "需要 Qt 6.11.2，实际为 ${qt_version}。" >&2; exit 1; }
qt_plugins="$("$qt_qmake" -query QT_INSTALL_PLUGINS)"
if [[ ! -f "$qt_plugins/imageformats/libqwebp.dylib" ]]; then
    echo "缺少 Qt WebP 插件：$qt_plugins/imageformats/libqwebp.dylib；需要官方 qtimageformats 模块。" >&2
    echo "请在 Qt MaintenanceTool 中为 Qt 6.11.2 勾选 Qt Image Formats，或用已安装的 aqt 执行：" >&2
    printf 'aqt install-qt mac desktop 6.11.2 clang_64 --noarchives --modules qtimageformats -O %q\n' "$(dirname "$(dirname "$QT_ROOT_DIR")")" >&2
    exit 1
fi
export QT_ROOT_DIR
printf '环境检测通过：Qt %s，路径 %s，WebP 插件已安装\n' "$qt_version" "$QT_ROOT_DIR"
