#!/usr/bin/env bash
set -euo pipefail

REPO="${REPO:-kevin-steve772/TExplorer}"
INSTALL_DIR="${HOME}/.texplorer"
TARGET="${INSTALL_DIR}/TExplorer"
LINK_NAME="TExplorer"
VERSION=""
ASSET=""
FORCE=0
TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"

usage() {
    cat <<'USAGE'
用法: ./install.sh [选项]

从 GitHub Release 下载 TExplorer 可执行文件，安装到 ~/.texplorer/，
并在 /bin/ 下创建符号链接。

选项:
  -h, --help              显示帮助
  -v, --version <tag>     指定 Release 标签（默认使用 latest）
  -a, --asset <name>      指定要下载的资源文件名（默认自动选择）
  -r, --repo <owner/repo> 指定 GitHub 仓库（默认 kevin-steve772/TExplorer）
  -n, --name <name>       指定 /bin 下的链接名（默认 TExplorer）
  -f, --force             允许覆盖 /bin 下已存在的普通文件

环境变量:
  GITHUB_TOKEN / GH_TOKEN 可选，用于避免 GitHub API 限流或访问私有仓库
  REPO                    可选，等价于 --repo

示例:
  ./install.sh
  ./install.sh --version v1.0.0
  ./install.sh --asset TExplorer-linux-x86_64
USAGE
}

info() {
    printf '==> %s\n' "$*"
}

warn() {
    printf '警告: %s\n' "$*" >&2
}

die() {
    printf '错误: %s\n' "$*" >&2
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -v|--version)
            [ $# -ge 2 ] || die "$1 需要一个参数"
            VERSION="$2"
            shift 2
            ;;
        -a|--asset)
            [ $# -ge 2 ] || die "$1 需要一个参数"
            ASSET="$2"
            shift 2
            ;;
        -r|--repo)
            [ $# -ge 2 ] || die "$1 需要一个参数"
            REPO="$2"
            shift 2
            ;;
        -n|--name)
            [ $# -ge 2 ] || die "$1 需要一个参数"
            LINK_NAME="$2"
            shift 2
            ;;
        -f|--force)
            FORCE=1
            shift
            ;;
        *)
            die "未知参数: $1（使用 --help 查看帮助）"
            ;;
    esac
done

[ -n "${HOME:-}" ] || die "HOME 未设置"
case "$REPO" in
    */*) ;;
    *) die "仓库格式应为 owner/repo: $REPO" ;;
esac
case "$LINK_NAME" in
    ""|*/*) die "链接名不能为空或包含 /: $LINK_NAME" ;;
esac

for cmd in curl install ln; do
    command -v "$cmd" >/dev/null 2>&1 || die "缺少命令: $cmd"
done

if ! command -v jq >/dev/null 2>&1 && ! command -v python3 >/dev/null 2>&1; then
    die "需要 jq 或 python3 来解析 GitHub API 响应"
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/texplorer-install.XXXXXX")" ||
    die "无法创建临时目录"
trap 'rm -rf "$TMP_DIR"' EXIT

curl_api() {
    if [ -n "$TOKEN" ]; then
        curl -H "Authorization: Bearer ${TOKEN}" "$@"
    else
        curl "$@"
    fi
}

list_assets() {
    local json="$1"
    if command -v jq >/dev/null 2>&1; then
        jq -r '.assets[]? | select(.name and .browser_download_url) | [.name, .browser_download_url] | @tsv' "$json"
    else
        python3 - "$json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)

for asset in data.get("assets", []):
    name = asset.get("name") or ""
    url = asset.get("browser_download_url") or ""
    if name and url:
        print(f"{name}\t{url}")
PY
    fi
}

pick_asset() {
    local list="$1"
    local best_name=""
    local best_url=""
    local best_score=-1
    local name url lname score arch

    arch="$(uname -m)"
    while IFS=$'\t' read -r name url; do
        [ -n "$name" ] || continue
        [ -n "$url" ] || continue
        lname="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"

        case "$lname" in
            *.deb|*.rpm|*.tar.gz|*.tgz|*.tar.xz|*.txz|*.zip|*.7z|*.exe|*.msi|*.sha256|*.sha512|*.asc|*.sig|*.txt|*.md|*.json|*source*|*src*)
                continue
                ;;
        esac

        case "$lname" in
            *darwin*|*macos*|*osx*|*windows*|*win32*|*win64*)
                continue
                ;;
        esac

        case "$arch" in
            x86_64|amd64)
                case "$lname" in
                    *aarch64*|*arm64*|*armv7*|*armv6*|*armhf*|*i386*|*i686*|*ia32*)
                        continue
                        ;;
                esac
                ;;
            aarch64|arm64)
                case "$lname" in
                    *x86*|*amd64*|*i386*|*i686*|*ia32*|*armv7*|*armv6*|*armhf*)
                        continue
                        ;;
                esac
                ;;
            armv7*|armv6*|arm)
                case "$lname" in
                    *x86*|*amd64*|*aarch64*|*arm64*|*i386*|*i686*|*ia32*)
                        continue
                        ;;
                esac
                ;;
            i386|i486|i586|i686|x86)
                case "$lname" in
                    *x86_64*|*amd64*|*aarch64*|*arm64*)
                        continue
                        ;;
                esac
                ;;
        esac

        score=0
        case "$lname" in
            texplorer) score=100 ;;
            *texplorer*) score=70 ;;
            *) score=30 ;;
        esac
        case "$lname" in
            *linux*) score=$((score + 30)) ;;
            *gnu*) score=$((score + 10)) ;;
        esac

        case "$arch" in
            x86_64|amd64)
                case "$lname" in
                    *x86_64*|*amd64*|*x64*) score=$((score + 25)) ;;
                esac
                ;;
            aarch64|arm64)
                case "$lname" in
                    *aarch64*|*arm64*) score=$((score + 25)) ;;
                esac
                ;;
            armv7*|armv6*|arm)
                case "$lname" in
                    *armv7*|*armv6*|*armhf*|*arm*) score=$((score + 25)) ;;
                esac
                ;;
            i386|i486|i586|i686|x86)
                case "$lname" in
                    *i386*|*i686*|*x86*) score=$((score + 25)) ;;
                esac
                ;;
        esac

        if [ "$score" -gt "$best_score" ]; then
            best_score="$score"
            best_name="$name"
            best_url="$url"
        fi
    done <<< "$list"

    [ -n "$best_url" ] || return 1
    printf '%s\t%s\n' "$best_name" "$best_url"
}

if [ -n "$VERSION" ]; then
    API_URL="https://api.github.com/repos/${REPO}/releases/tags/${VERSION}"
    info "获取 Release: ${VERSION}"
else
    API_URL="https://api.github.com/repos/${REPO}/releases/latest"
    info "获取最新 Release"
fi

if ! curl_api -fsSL --retry 3 --retry-delay 2 --connect-timeout 15 \
    -H "Accept: application/vnd.github+json" \
    -o "$TMP_DIR/release.json" "$API_URL"; then
    die "无法获取 Release 信息。请确认仓库已有 Release，并检查网络或 GitHub API 限流；可设置 GITHUB_TOKEN。"
fi

if ! ASSETS="$(list_assets "$TMP_DIR/release.json")"; then
    die "解析 Release 信息失败"
fi
[ -n "$ASSETS" ] || die "该 Release 没有可下载的资源文件"

if [ -n "$ASSET" ]; then
    CHOSEN="$(printf '%s\n' "$ASSETS" | awk -F '\t' -v want="$ASSET" '$1 == want { print; exit }')"
    [ -n "$CHOSEN" ] || die "Release 中未找到资源: $ASSET"
else
    CHOSEN="$(pick_asset "$ASSETS")" ||
        die "未找到适合当前系统（$(uname -m)）的可执行资源；可用 --asset 手动指定。"
fi

ASSET_NAME="${CHOSEN%%$'\t'*}"
ASSET_URL="${CHOSEN#*$'\t'}"
[ -n "$ASSET_NAME" ] && [ -n "$ASSET_URL" ] || die "无法解析资源信息"

info "下载 ${ASSET_NAME}"
if [ -t 1 ]; then
    curl_api -fL --retry 3 --retry-delay 2 --connect-timeout 15 \
        --progress-bar -o "$TMP_DIR/download" "$ASSET_URL" ||
        die "下载失败: $ASSET_URL"
else
    curl_api -fsSL --retry 3 --retry-delay 2 --connect-timeout 15 \
        -o "$TMP_DIR/download" "$ASSET_URL" ||
        die "下载失败: $ASSET_URL"
fi

[ -s "$TMP_DIR/download" ] || die "下载的文件为空"

MAGIC="$(od -An -tx1 -N4 "$TMP_DIR/download" | tr -d ' \n')"
case "$MAGIC" in
    7f454c46|2321*) ;;
    *) die "下载的文件不是可执行程序（可能是错误页面或压缩包）" ;;
esac

info "安装到 ${INSTALL_DIR}"
install -d -m 755 "$INSTALL_DIR" || die "无法创建目录: $INSTALL_DIR"
install -m 755 "$TMP_DIR/download" "$TARGET" || die "无法写入程序: $TARGET"

LINK="/bin/${LINK_NAME}"
[ -d /bin ] || die "/bin 不存在"
if { [ -e "$LINK" ] || [ -L "$LINK" ]; } && [ ! -L "$LINK" ] && [ "$FORCE" -ne 1 ]; then
    die "${LINK} 已存在且不是符号链接；使用 --force 覆盖"
fi

if [ -w /bin ]; then
    ln -sfn "$TARGET" "$LINK" || die "创建符号链接失败: $LINK"
else
    command -v sudo >/dev/null 2>&1 ||
        die "没有写入 /bin 的权限，且未找到 sudo。请手动执行: sudo ln -sfn '$TARGET' '$LINK'"
    info "创建符号链接需要管理员权限: ${LINK}"
    sudo ln -sfn "$TARGET" "$LINK" || die "创建符号链接失败: $LINK"
fi

[ -x "$LINK" ] || die "符号链接创建失败: $LINK"

printf '\n安装完成。\n'
printf '  程序: %s\n' "$TARGET"
printf '  链接: %s -> %s\n' "$LINK" "$TARGET"
printf '  运行: %s [目录]\n' "$LINK_NAME"
