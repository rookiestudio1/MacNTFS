# MacNTFS

用 Apple **FSKit** 搭配 **libntfs-3g** 讓 macOS 15.4+ 讀寫 NTFS 的 Xcode 專案。

- 偵測到 NTFS 硬碟時自動掛載到 `/Volumes/<卷標>`（透過 DiskArbitration + FSKit 探測順序）
- 可同時掛載多顆硬碟
- 選單列 App 提供：手動掛載、卸載、改為唯讀重新掛載、改回讀寫

## 專案結構

```
project.yml                 XcodeGen 描述檔（修改簽章、Bundle ID 請改這裡）
MacNTFS.xcodeproj           由 `xcodegen generate` 產生
MacNTFS/                    SwiftUI 選單列 App（DiskArbitration 監看、diskutil 掛載/卸載）
NTFSFSExtension/            FSKit 檔案系統延伸功能（Objective-C + C，Swift 入口）
  Bridge/ntfs_bridge.[ch]   libntfs-3g 的 C 橋接層（以 MFT 編號為單位）
  NTFSBlockDevice.m         FSBlockDeviceResource → libntfs-3g 裝置回呼（含磁區對齊）
  NTFSFileSystem.m          probe / load / unload / fsck
  NTFSVolume.m              FSVolume 操作（讀寫、目錄、xattr、重新命名…）
Tools/ntfs-bridge-test/     以映像檔測試橋接層的 CLI
Scripts/build-ntfs3g.sh     下載並編譯 libntfs-3g（arm64 + x86_64 universal 靜態庫）
ThirdParty/ntfs-3g/         編譯好的 libntfs-3g.a 與標頭（LGPL 2.1）
docs/superpowers/specs/     設計文件
```

## 建置

需求：macOS 15.4+（開發時使用 macOS 26.5 / Xcode 26.6）、Xcode 16.3+、Homebrew 的 `xcodegen`（只在修改 `project.yml` 後需要）。

```bash
# 1.（可選）重新產生 Xcode 專案
brew install xcodegen
xcodegen generate

# 2. 編譯 libntfs-3g（第一次建置時 Xcode 的 pre-build script 也會自動執行）
Scripts/build-ntfs3g.sh

# 3. 用 Xcode 開啟 MacNTFS.xcodeproj，或：
xcodebuild -scheme MacNTFS -configuration Release build
```

### 簽章（必要）

FSKit 延伸功能需要 `com.apple.developer.fskit.fsmodule` entitlement，macOS 只會載入**由 provisioning profile 授權**的延伸功能，ad-hoc 簽章不會被載入。請在 `project.yml` 的 `DEVELOPMENT_TEAM` 填入你的 Team ID（或在 Xcode 的 Signing & Capabilities 選擇 Team），並確認 App ID 已啟用 FSKit 能力，再 `xcodegen generate`。

Bundle ID 預設為 `com.rookiestudio.MacNTFS` 與 `com.rookiestudio.MacNTFS.NTFSFSExtension`，可在 `project.yml` 一併修改（App 內 `ExtensionStatus.swift` 的常數也要跟著改）。

## 安裝與啟用

1. 把 `MacNTFS.app` 放到 `/Applications` 並執行一次。
2. 到「系統設定 > 一般 > 登入項目與延伸功能 > 檔案系統延伸功能」啟用 **MacNTFS**。
3. 插入 NTFS 硬碟：DiskArbitration 會依 `FSProbeOrder`（本延伸功能為 500/1500/3500，低於內建 `ntfs.fs` 的 1000/2000/4000）優先用 MacNTFS 探測並自動掛載到 `/Volumes`。
4. 若仍被內建唯讀驅動程式搶先掛載（`DAVolumeKind == ntfs`），選單列 App 會顯示「系統唯讀掛載」，可按「改用 MacNTFS 掛載」——這條備援路徑會先 `diskutil unmount`，再以管理員權限執行 `mount -F -t ntfs3g`。

手動操作對應的指令：

```bash
diskutil mount disk4s1                 # 讀寫掛載（經 diskarbitrationd → FSKit）
diskutil mount readOnly disk4s1        # 唯讀掛載
diskutil unmount disk4s1               # 卸載
sudo mount -F -t ntfs3g -o rdonly /dev/disk4s1 /Volumes/NTFS   # 備援：直接呼叫 FSKit
```

## 測試

### 橋接層（不需要簽章與 FSKit）

用 ntfs-3g 附的 `mkntfs` 建立映像檔後執行 `ntfs-bridge-test`：

```bash
# 建立 64 MB 測試映像檔（mkntfs 可從 ThirdParty/build 內的原始碼另外編譯 ntfsprogs 取得）
dd if=/dev/zero of=test.img bs=1m count=64
mkntfs -F -f -L TestNTFS test.img

xcodebuild -scheme ntfs-bridge-test -configuration Debug build
"$(xcodebuild -scheme ntfs-bridge-test -showBuildSettings | awk '/ BUILT_PRODUCTS_DIR/{print $3}')/ntfs-bridge-test" test.img
```

測試涵蓋：probe、讀寫掛載、statfs、mkdir、建立/讀寫/未對齊寫入/truncate、不分大小寫查找、xattr（具名串流）、符號連結、硬連結、Windows 禁用字元、時間戳與旗標、目錄列舉（完整與分段 cookie）、重新命名與覆蓋、刪除、卷標、唯讀重掛與 EROFS。

### 端對端

```bash
# 用映像檔模擬硬碟
hdiutil attach -nomount test.img          # 會印出 /dev/diskN
diskutil mount diskN                      # 應由 MacNTFS 掛載到 /Volumes/TestNTFS
mount | grep ntfs3g
diskutil unmount diskN && hdiutil detach diskN

# 除錯紀錄
log stream --predicate 'subsystem == "com.apple.FSKit" OR subsystem == "com.rookiestudio.MacNTFS"'
pluginkit -m -p com.apple.fskit.fsmodule  # 確認延伸功能已註冊
```

## 疑難排解

### `Module com.rookiestudio.MacNTFS.NTFSFSExtension is disabled!`

延伸功能已正確安裝與簽章，但還沒在系統設定裡啟用。到「系統設定 > 一般 > 登入項目與延伸功能」，找到「檔案系統模組」（File System Modules）右側的 ⓘ 按鈕，把 **MacNTFS 檔案系統延伸功能** 的開關打開。

這個啟用狀態寫在 `~/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist`，只能由系統設定的開關寫入，`pluginkit -e use` 對它無效。

### 確認延伸功能是否被系統看見

```bash
pluginkit -m -p com.apple.fskit.fsmodule -vvv   # 應列出 com.rookiestudio.MacNTFS.NTFSFSExtension
plutil -p ~/Library/Group\ Containers/group.com.apple.fskit.settings/enabledModules.plist   # 啟用後才會出現
plutil -p ~/Library/Group\ Containers/group.com.apple.fskit.settings/probeOrder.plist       # 探測順序
```

若 `pluginkit` 沒列出，通常是 App 尚未被 LaunchServices 註冊：

```bash
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f /Applications/MacNTFS.app
```

### 簽章檢查

```bash
codesign -d --entitlements - --xml /Applications/MacNTFS.app/Contents/Extensions/NTFSFSExtension.appex | plutil -p -
```

必須看到 `com.apple.developer.fskit.fsmodule => true`，且該 appex 內要有 `Contents/embedded.provisionprofile`。缺少 provisioning profile 時 AMFI 不會載入延伸功能。

## 設計重點

- **不長期持有 libntfs-3g inode**：每個操作 `ntfs_inode_open` → 操作 → `ntfs_inode_close`，避免同一 inode 重複開啟造成 MFT 記錄不一致（與 ntfs-3g FUSE 驅動相同）。
- **不分大小寫且保留大小寫**：清除 libntfs-3g 的大小寫敏感旗標，但用恆等 locase 表避免列舉時被轉成小寫。
- **磁區對齊**：`FSBlockDeviceResource` 要求磁區對齊的讀寫，橋接層對未對齊要求做讀-改-寫。
- **xattr → 具名 `$DATA` 串流**：macOS 的 `com.apple.*` 延伸屬性會存成 NTFS 具名串流。
- **權限**：不做 NTFS 使用者對應；uid/gid 預設 99（等同 Finder「忽略此卷的擁有權」），`FILE_ATTR_READONLY` 對應去掉寫入位元，`FILE_ATTR_HIDDEN` 對應 `UF_HIDDEN`。
- **休眠的 Windows 卷**：探測仍回報可用，掛載時 libntfs-3g 自動退回唯讀。

## 已知限制

- 不支援 Kernel-offloaded I/O，大檔案吞吐量受 XPC 往返限制。
- 不支援格式化與修復（`fsck` 只檢查能否解析與是否休眠）。
- 不支援 NTFS 壓縮 / 加密檔案的寫入。
- 重新命名為 link + unlink，非原子操作。

## 授權

libntfs-3g 為 LGPL 2.1（見 `ThirdParty/ntfs-3g/LICENSE.LGPL`），以靜態連結方式使用；本專案自有原始碼授權由專案擁有者決定。
