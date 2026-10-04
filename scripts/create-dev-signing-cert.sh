#!/usr/bin/env bash
# 仅由开发者显式执行一次；构建和 CI 不会调用此脚本。
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo "此脚本仅适用于 macOS。" >&2; exit 1; }
for tool in openssl security; do
    command -v "$tool" >/dev/null || { echo "缺少 ${tool}。" >&2; exit 1; }
done
identity="WaibuSnap Dev"
identities="$(security find-identity -v -p codesigning)"
if [[ "$identities" == *"\"$identity\""* ]]; then
    echo "签名身份「${identity}」已存在，无需重新创建。"
    exit 0
fi
# 信任被取消后仍保留原身份，避免重新生成证书叶、破坏已有授权。
identities="$(security find-identity -p codesigning)"
if [[ "$identities" == *"\"$identity\""* ]]; then
    echo "签名身份「${identity}」已存在，但尚未通过有效身份校验。"
    echo "请在钥匙串访问中核对有效期与信任：双击证书 → 显示简介 → 信任 → 代码签名：始终信任。"
    exit 0
fi
login_keychain="$(security login-keychain -d user | sed -e 's/^[[:space:]]*"//' -e 's/"[[:space:]]*$//' || true)"
# 部分主机未登记 login-keychain 偏好，仍使用当前用户的登录钥匙串。
login_keychain="${login_keychain:-$HOME/Library/Keychains/login.keychain-db}"
[[ -f "$login_keychain" ]] || { echo "未找到登录钥匙串：${login_keychain}" >&2; exit 1; }
umask 077
temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/waibusnap-signing.XXXXXX")"
trap 'rm -rf "$temp_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$temp_dir/openssl.cnf" <<'EOF'
[req]
prompt = no
distinguished_name = subject
x509_extensions = signing
[subject]
CN = WaibuSnap Dev
[signing]
basicConstraints = CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF
echo "创建十年有效的本机开发证书「${identity}」。"
openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -config "$temp_dir/openssl.cnf" -keyout "$temp_dir/key.pem" -out "$temp_dir/cert.pem"
openssl rand -hex 24 > "$temp_dir/p12-password"
# 显式指定兼容系统 LibreSSL 与 security import 的 PKCS#12 编码。
openssl pkcs12 -export -name "$identity" -inkey "$temp_dir/key.pem" \
    -in "$temp_dir/cert.pem" -out "$temp_dir/identity.p12" \
    -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 \
    -passout "file:$temp_dir/p12-password"
echo "导入登录钥匙串；如 codesign 弹出一次性钥匙串授权提示，请点「始终允许」。"
security import "$temp_dir/identity.p12" -k "$login_keychain" -f pkcs12 \
    -P "$(cat "$temp_dir/p12-password")" -T /usr/bin/codesign
echo "建议为代码签名设置用户域信任；系统可能要求认证（无需 sudo）。"
if security add-trusted-cert -r trustRoot -p codeSign -k "$login_keychain" "$temp_dir/cert.pem"; then
    echo "用户域代码签名信任已设置。"
else
    echo "自动信任未完成；证书已导入，签名本身不依赖此信任步骤。"
    echo "可在钥匙串访问 → 登录 → WaibuSnap Dev，双击证书 → 显示简介 → 信任 → 代码签名：始终信任。"
fi
cat <<'EOF'
创建完成；临时私钥、证书和 p12 将随脚本退出清理。
校验身份：security find-identity -v -p codesigning
删除证书及私钥（同时移除用户信任）：security delete-identity -c "WaibuSnap Dev" -t
首次切换签名后需做一次性屏幕录制重新授权：
  先退出 WaibuSnap，由用户执行 tccutil reset ScreenCapture local.waibusnap.dev，或在系统设置移除旧条目。
  然后 bash scripts/run-app.sh 普通启动 → F1 / 托盘「截图」→ 系统弹窗「允许」；系统要求时重启应用。
  此后使用同一证书和 bundle id 重建，无需再次授权；更换证书或 bundle id 需重新授权。
EOF
