#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
build_dir="${WAIBUSNAP_BUILD_DIR:-$repo_root/build/macos}"
app="$build_dir/bin/WaibuSnap.app/Contents/MacOS/WaibuSnap"
[[ -x "$app" ]] || { echo "请先执行 build-project.sh。" >&2; exit 1; }
exec "$app" "$@"
