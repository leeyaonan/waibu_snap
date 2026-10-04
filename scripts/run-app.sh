#!/usr/bin/env bash
# macOS 27 起终端直启不会继承应用的屏幕录制授权，必须经 LaunchServices 启动。
# 阻塞到应用退出；Ctrl+C 只结束脚本与日志跟随，应用继续留在托盘。
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
build_dir="${WAIBUSNAP_BUILD_DIR:-$repo_root/build/macos}"
app="$build_dir/bin/WaibuSnap.app"
[[ -x "$app/Contents/MacOS/WaibuSnap" ]] || { echo "请先执行 build-project.sh。" >&2; exit 1; }
if pgrep -x WaibuSnap >/dev/null; then
    echo "WaibuSnap 已在运行；请先从菜单栏退出，或直接使用它"
    exit 0
else
    pgrep_status=$?
    [[ "$pgrep_status" == 1 ]] || { echo "无法检查 WaibuSnap 实例。" >&2; exit 1; }
fi
mkdir -p "$build_dir/logs"
stdout_log="$build_dir/logs/run-app.stdout.log"
stderr_log="$build_dir/logs/run-app.stderr.log"
: > "$stdout_log"
: > "$stderr_log"
launcher_pid=""
tail_pid=""
cleanup() {
    # 只回收本脚本的 open 等待器和 tail，不向应用进程发送信号。
    for child_pid in "$tail_pid" "$launcher_pid"; do
        if [[ -n "$child_pid" ]]; then
            kill "$child_pid" 2>/dev/null || true
            wait "$child_pid" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
open -W -n "$app" --stdout "$stdout_log" --stderr "$stderr_log" --args "$@" &
launcher_pid=$!
tail -n +1 -f "$stderr_log" &
tail_pid=$!
status=0
wait "$launcher_pid" || status=$?
kill "$tail_pid" 2>/dev/null || true
wait "$tail_pid" 2>/dev/null || true
tail_pid=""
exit "$status"
