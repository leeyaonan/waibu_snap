#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
source "$script_dir/prepare-environment.sh"
build_type="${WAIBUSNAP_BUILD_TYPE:-Release}"
case "$build_type" in Debug|Release|RelWithDebInfo|MinSizeRel) ;; *) echo "不支持的构建配置。" >&2; exit 1 ;; esac
build_dir="${WAIBUSNAP_BUILD_DIR:-$repo_root/build/macos}"
cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    "-DCMAKE_PREFIX_PATH=$QT_ROOT_DIR" "-DCMAKE_BUILD_TYPE=$build_type" \
    "-DCMAKE_OSX_ARCHITECTURES=${MACOS_ARCHITECTURES:-$(uname -m)}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel
