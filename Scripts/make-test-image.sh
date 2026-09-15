#!/bin/bash
# 產生 ntfs-bridge-test / bench 用的 NTFS 測試映像檔。
#
# 為此需要 mkntfs，但 build-ntfs3g.sh 為了產出給 App 用的 universal 靜態函式庫，
# 帶了 --disable-ntfsprogs。這裡另外做一次「只給本機用」的 configure，單獨編出 mkntfs，
# 產物放在 ThirdParty/build/host-tools/，不會進入 App 的建置流程。
#
# 用法：Scripts/make-test-image.sh [大小MB] [輸出路徑]
#   預設：512 MB，輸出到 build/bench/bench.img
set -euo pipefail

SIZE_MB="${1:-512}"
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_IMG="${2:-$ROOT_DIR/build/bench/bench.img}"

NTFS3G_VERSION="${NTFS3G_VERSION:-2026.7.7}"
NTFS3G_URL="https://tuxera.com/opensource/ntfs-3g_ntfsprogs-${NTFS3G_VERSION}.tgz"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/ThirdParty/build}"
HOST_DIR="$BUILD_DIR/host-tools"
MKNTFS="$HOST_DIR/bin/mkntfs"
SRC_DIR="$BUILD_DIR/ntfsprogs-host-$NTFS3G_VERSION"

# ---- 1. 準備 mkntfs（只在還沒有的時候編）----
if [[ ! -x "$MKNTFS" ]]; then
  echo "==> 編譯 mkntfs（本機架構，僅供測試使用）"
  mkdir -p "$BUILD_DIR"
  TARBALL="$BUILD_DIR/ntfs-3g-$NTFS3G_VERSION.tgz"
  if [[ ! -f "$TARBALL" ]]; then
    echo "下載 $NTFS3G_URL"
    curl -fsSL -o "$TARBALL" "$NTFS3G_URL"
  fi
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  tar xzf "$TARBALL" -C "$SRC_DIR" --strip-components=1
  (
    cd "$SRC_DIR"
    ./configure \
      --prefix="$HOST_DIR" \
      --disable-ntfs-3g \
      --enable-ntfsprogs \
      --disable-shared \
      --enable-static \
      --disable-plugins \
      --disable-mtab \
      --without-uuid \
      --disable-dependency-tracking \
      >"$BUILD_DIR/configure-host.log" 2>&1
    make -j"$(sysctl -n hw.ncpu)" -C libntfs-3g >"$BUILD_DIR/make-host.log" 2>&1
    make -C ntfsprogs mkntfs >>"$BUILD_DIR/make-host.log" 2>&1
    mkdir -p "$HOST_DIR/bin"
    cp ntfsprogs/mkntfs "$HOST_DIR/bin/"
  )
  echo "    產出 $MKNTFS"
fi

# ---- 2. 產生映像檔 ----
mkdir -p "$(dirname "$OUT_IMG")"
rm -f "$OUT_IMG"
echo "==> 建立 ${SIZE_MB} MB 映像檔 $OUT_IMG"
# 用 seek 產生稀疏檔，避免真的寫入 512 MB 的零
dd if=/dev/zero of="$OUT_IMG" bs=1m count=0 seek="$SIZE_MB" 2>/dev/null

echo "==> 格式化為 NTFS"
"$MKNTFS" -F -f -L BenchNTFS "$OUT_IMG" >/dev/null

echo "完成：$OUT_IMG"
