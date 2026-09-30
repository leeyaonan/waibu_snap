#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
command -v clang-format >/dev/null || { echo "请按 scripts/README.md 准备 clang-format 18.1.8。" >&2; exit 1; }
[[ "$(clang-format --version)" =~ version\ 18\.1\.8([[:space:]]|$) ]] || { echo "格式工具必须为 clang-format 18.1.8。" >&2; exit 1; }
while IFS= read -r -d '' source_file; do
    clang-format --dry-run --Werror "$source_file"
done < <(find "$repo_root/src" "$repo_root/tests" -type f     \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) -print0)
