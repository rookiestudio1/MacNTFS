#!/bin/bash
# 由專案根目錄的 ntfs.png 產生 App icon 的 asset catalog
# 用法：Scripts/make-appicon.sh [來源圖檔]
# 預設來源：<專案根>/ntfs.png
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${1:-$ROOT_DIR/ntfs.png}"
ICONSET="$ROOT_DIR/MacNTFS/Assets.xcassets/AppIcon.appiconset"

if [[ ! -f "$SOURCE" ]]; then
  echo "找不到來源圖檔：$SOURCE" >&2
  exit 1
fi

SRC_W=$(sips -g pixelWidth "$SOURCE" | awk '/pixelWidth/{print $2}')
SRC_H=$(sips -g pixelHeight "$SOURCE" | awk '/pixelHeight/{print $2}')
echo "來源：${SOURCE}（${SRC_W}x${SRC_H}）"
if [[ "$SRC_W" -lt 1024 || "$SRC_H" -lt 1024 ]]; then
  echo "提醒：來源小於 1024x1024，較大的尺寸會由放大產生，邊緣可能較模糊。"
fi

rm -rf "$ICONSET"
mkdir -p "$ICONSET"

# macOS App icon 需要的尺寸：檔名 對應像素
render() {
  local name="$1" size="$2"
  sips -s format png -z "$size" "$size" "$SOURCE" --out "$ICONSET/$name" >/dev/null
}

render icon_16x16.png        16
render icon_16x16@2x.png     32
render icon_32x32.png        32
render icon_32x32@2x.png     64
render icon_128x128.png      128
render icon_128x128@2x.png   256
render icon_256x256.png      256
render icon_256x256@2x.png   512
render icon_512x512.png      512
render icon_512x512@2x.png   1024

cat > "$ICONSET/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "icon_16x16.png",      "idiom" : "mac", "scale" : "1x", "size" : "16x16" },
    { "filename" : "icon_16x16@2x.png",   "idiom" : "mac", "scale" : "2x", "size" : "16x16" },
    { "filename" : "icon_32x32.png",      "idiom" : "mac", "scale" : "1x", "size" : "32x32" },
    { "filename" : "icon_32x32@2x.png",   "idiom" : "mac", "scale" : "2x", "size" : "32x32" },
    { "filename" : "icon_128x128.png",    "idiom" : "mac", "scale" : "1x", "size" : "128x128" },
    { "filename" : "icon_128x128@2x.png", "idiom" : "mac", "scale" : "2x", "size" : "128x128" },
    { "filename" : "icon_256x256.png",    "idiom" : "mac", "scale" : "1x", "size" : "256x256" },
    { "filename" : "icon_256x256@2x.png", "idiom" : "mac", "scale" : "2x", "size" : "256x256" },
    { "filename" : "icon_512x512.png",    "idiom" : "mac", "scale" : "1x", "size" : "512x512" },
    { "filename" : "icon_512x512@2x.png", "idiom" : "mac", "scale" : "2x", "size" : "512x512" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON

echo "已產生：$ICONSET"
ls -1 "$ICONSET"
