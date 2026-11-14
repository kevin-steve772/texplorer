#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${HOME}/.texplorer"
TARGET="${INSTALL_DIR}/TExplorer"
LINK_NAME="TExplorer"
FORCE=0
PURGE=0

usage() {
    cat <<'USAGE'
用法: ./uninstall.sh [选项]

卸载 TExplorer：删除 /bin/ 下的符号链接、~/.texplorer/ 中的程序，
并在目录为空时删除 ~/.texplorer/。

选项:
  -h, --help          显示帮助
  -n, --name <name>   指定 /bin 下的链接名（默认 TExplorer）
  -p, --purge         递归删除整个 ~/.texplorer/ 目录
  -f, --force         即使链接指向其他位置，也强制删除 /bin 下的链接

示例:
  ./uninstall.sh
  ./uninstall.sh --purge
  ./uninstall.sh --name texplorer
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
        -n|--name)
            [ $# -ge 2 ] || die "$1 需要一个参数"
            LINK_NAME="$2"
            shift 2
            ;;
        -p|--purge)
            PURGE=1
            shift
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
case "$LINK_NAME" in
    ""|*/*) die "链接名不能为空或包含 /: $LINK_NAME" ;;
esac

LINK="/bin/${LINK_NAME}"

remove_path() {
    local path="$1"
    if [ -w "$(dirname "$path")" ]; then
        rm -f "$path" || die "删除失败: $path"
    else
        command -v sudo >/dev/null 2>&1 ||
            die "没有权限删除 ${path}，且未找到 sudo。请手动执行: sudo rm -f '${path}'"
        info "删除 ${path} 需要管理员权限"
        sudo rm -f "$path" || die "删除失败: $path"
    fi
}

link_matches_target() {
    local dest real_link real_target
    [ -L "$LINK" ] || return 1
    dest="$(readlink "$LINK" 2>/dev/null || true)"
    [ "$dest" = "$TARGET" ] && return 0
    real_link="$(readlink -f "$LINK" 2>/dev/null || true)"
    real_target="$(readlink -f "$TARGET" 2>/dev/null || true)"
    [ -n "$real_link" ] && [ -n "$real_target" ] && [ "$real_link" = "$real_target" ]
}

if [ -L "$LINK" ]; then
    if link_matches_target || [ "$FORCE" -eq 1 ]; then
        remove_path "$LINK"
        info "已删除符号链接: $LINK"
    else
        warn "$LINK 指向其他位置，已跳过；使用 --force 可强制删除"
    fi
elif [ -e "$LINK" ]; then
    if [ "$FORCE" -eq 1 ]; then
        remove_path "$LINK"
        info "已删除文件: $LINK"
    else
        warn "$LINK 不是符号链接，已跳过；使用 --force 可强制删除"
    fi
else
    info "未找到符号链接: $LINK"
fi

if [ -e "$TARGET" ] || [ -L "$TARGET" ]; then
    rm -f "$TARGET" || die "删除失败: $TARGET"
    info "已删除程序: $TARGET"
else
    info "未找到程序: $TARGET"
fi

if [ "$PURGE" -eq 1 ]; then
    if [ -d "$INSTALL_DIR" ]; then
        rm -rf "$INSTALL_DIR" || die "删除目录失败: $INSTALL_DIR"
        info "已删除目录: $INSTALL_DIR"
    fi
elif [ -d "$INSTALL_DIR" ]; then
    if rmdir "$INSTALL_DIR" 2>/dev/null; then
        info "已删除空目录: $INSTALL_DIR"
    else
        info "目录非空，已保留: $INSTALL_DIR（使用 --purge 可递归删除）"
    fi
fi

printf '\n卸载完成。\n'
