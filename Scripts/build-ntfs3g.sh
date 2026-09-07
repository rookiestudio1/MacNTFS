#!/bin/bash
# 下載並編譯 libntfs-3g 靜態函式庫（arm64 + x86_64 universal）
# 產出：ThirdParty/ntfs-3g/lib/libntfs-3g.a 與 ThirdParty/ntfs-3g/include/ntfs-3g/*.h
# 用法：Scripts/build-ntfs3g.sh [--force]
set -euo pipefail

NTFS3G_VERSION="${NTFS3G_VERSION:-2026.7.7}"
NTFS3G_URL="https://tuxera.com/opensource/ntfs-3g_ntfsprogs-${NTFS3G_VERSION}.tgz"
MIN_MACOS="${MIN_MACOS:-15.4}"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT_DIR/ThirdParty/ntfs-3g"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/ThirdParty/build}"
SRC_DIR="$BUILD_DIR/ntfs-3g-$NTFS3G_VERSION"

if [[ "${1:-}" != "--force" && -f "$OUT_DIR/lib/libntfs-3g.a" && -f "$OUT_DIR/include/ntfs-3g/volume.h" ]]; then
  echo "libntfs-3g 已存在，略過編譯（加 --force 可強制重建）"
  exit 0
fi

mkdir -p "$BUILD_DIR"
TARBALL="$BUILD_DIR/ntfs-3g-$NTFS3G_VERSION.tgz"
if [[ ! -f "$TARBALL" ]]; then
  echo "下載 $NTFS3G_URL"
  curl -fsSL -o "$TARBALL" "$NTFS3G_URL"
fi
rm -rf "$SRC_DIR"
tar xzf "$TARBALL" -C "$BUILD_DIR"

# 針對單一架構進行 configure + make
build_arch() {
  local arch="$1"
  local prefix="$BUILD_DIR/install-$arch"
  local host_flag=""
  # 交叉編譯 x86_64 時需要指定 host
  if [[ "$arch" == "x86_64" && "$(uname -m)" != "x86_64" ]]; then
    host_flag="--host=x86_64-apple-darwin"
  elif [[ "$arch" == "arm64" && "$(uname -m)" != "arm64" ]]; then
    host_flag="--host=aarch64-apple-darwin"
  fi
  echo "==> 編譯 libntfs-3g ($arch)"
  (
    cd "$SRC_DIR"
    make distclean >/dev/null 2>&1 || true
    ./configure \
      --prefix="$prefix" \
      --disable-ntfs-3g \
      --disable-ntfsprogs \
      --disable-shared \
      --enable-static \
      --disable-plugins \
      --disable-mtab \
      --without-uuid \
      --disable-dependency-tracking \
      $host_flag \
      CC="$(xcrun -f clang)" \
      CFLAGS="-arch $arch -mmacosx-version-min=$MIN_MACOS -O2 -isysroot $(xcrun --sdk macosx --show-sdk-path)" \
      LDFLAGS="-arch $arch -mmacosx-version-min=$MIN_MACOS" \
      >"$BUILD_DIR/configure-$arch.log" 2>&1
    make -j"$(sysctl -n hw.ncpu)" -C libntfs-3g >"$BUILD_DIR/make-$arch.log" 2>&1
    # ntfs-3g 的 make install hook 假設有 .so，靜態編譯時會失敗，因此直接複製產物
    mkdir -p "$prefix/lib" "$prefix/include/ntfs-3g"
    cp libntfs-3g/.libs/libntfs-3g.a "$prefix/lib/"
    cp include/ntfs-3g/*.h "$prefix/include/ntfs-3g/"
    cp config.h "$prefix/include/ntfs-3g/config.h"
  )
}

build_arch arm64
build_arch x86_64

echo "==> 合併為 universal 靜態函式庫"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/lib" "$OUT_DIR/include"
lipo -create \
  "$BUILD_DIR/install-arm64/lib/libntfs-3g.a" \
  "$BUILD_DIR/install-x86_64/lib/libntfs-3g.a" \
  -output "$OUT_DIR/lib/libntfs-3g.a"
cp -R "$BUILD_DIR/install-arm64/include/ntfs-3g" "$OUT_DIR/include/"
cp "$SRC_DIR/COPYING.LIB" "$OUT_DIR/LICENSE.LGPL"
echo "完成：$OUT_DIR/lib/libntfs-3g.a"
lipo -info "$OUT_DIR/lib/libntfs-3g.a"
