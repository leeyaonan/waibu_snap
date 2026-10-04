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

app="$build_dir/bin/WaibuSnap.app"
signing_identity="-"
if [[ "${CI:-}" == true || "${GITHUB_ACTIONS:-}" == true ]]; then
    echo "CI 保持 ad-hoc 签名。"
elif [[ "${WAIBUSNAP_CODESIGN_IDENTITY+x}" == x &&
        ( -z "$WAIBUSNAP_CODESIGN_IDENTITY" || "$WAIBUSNAP_CODESIGN_IDENTITY" == none ) ]]; then
    echo "已跳过证书签名，恢复 ad-hoc 签名。"
else
    requested_identity="${WAIBUSNAP_CODESIGN_IDENTITY-WaibuSnap Dev}"
    if ! identities="$(security find-identity -p codesigning)"; then
        echo "无法查询代码签名身份。" >&2
        exit 1
    fi
    # 按完整名称或证书 SHA-1 匹配；包括未设置信任的身份，签名不依赖信任。
    identity_hashes="$(printf '%s\n' "$identities" | awk -v requested="$requested_identity" '
        /^[[:space:]]*[0-9]+\)/ {
            name = $0
            sub(/^[^"]*"/, "", name)
            sub(/".*$/, "", name)
            if (name == requested || toupper($2) == toupper(requested)) print $2
        }' | sort -u)"
    identity_count="$(printf '%s\n' "$identity_hashes" | awk 'NF { n++ } END { print n+0 }')"
    if [[ "$identity_count" == 0 ]]; then
        if [[ "${WAIBUSNAP_CODESIGN_IDENTITY+x}" == x ]]; then
            echo "指定签名身份不存在：${requested_identity}" >&2
            exit 1
        fi
        echo "未检测到 WaibuSnap Dev，保持 ad-hoc；稳定签名配置见 scripts/README.md「本机稳定签名」。"
    elif [[ "$identity_count" != 1 ]]; then
        echo "签名身份名称不唯一：${requested_identity}；请用 WAIBUSNAP_CODESIGN_IDENTITY 指定证书 SHA-1。" >&2
        exit 1
    else
        signing_identity="$identity_hashes"
        echo "使用代码签名身份：${requested_identity}（${signing_identity}）"
    fi
fi
# 增量构建可能不重链接；跳过证书签名时也显式重签为 ad-hoc。
if ! codesign --force --sign "$signing_identity" --identifier local.waibusnap.dev \
        --timestamp=none "$app"; then
    echo "WaibuSnap.app 签名失败，不回退到其他身份。" >&2
    exit 1
fi
if ! codesign --verify --verbose=2 "$app"; then
    echo "WaibuSnap.app 签名校验失败。" >&2
    exit 1
fi
signature_summary="$(codesign -dvvv "$app" 2>&1)"
printf '%s\n' "$signature_summary" | awk '/^(Authority=|Identifier=|CDHash=|Signature=)/'
requirement="$(codesign -d -r- "$app" 2>&1)"
printf '%s\n' "$requirement"
if [[ "$signing_identity" != - ]] &&
        ! printf '%s\n' "$requirement" | LC_ALL=C grep -Eq \
        'designated => identifier "local\.waibusnap\.dev" and certificate leaf = H"[[:xdigit:]]{40}"'; then
    echo "签名的 designated requirement 不是预期的 identifier + certificate leaf 形式。" >&2
    exit 1
fi
