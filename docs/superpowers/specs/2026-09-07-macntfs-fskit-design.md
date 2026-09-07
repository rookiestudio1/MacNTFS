# MacNTFS 設計文件（FSKit + libntfs-3g）

日期：2026-09-07
狀態：已依此實作完成（本文件同步反映實作時的最終決策），待使用者審閱調整

## 1. 目標

在 macOS 15.4+（開發機為 macOS 26.5）上，用 Apple FSKit 搭配 libntfs-3g 提供 NTFS 讀寫能力：

- 偵測到新的 NTFS 硬碟時自動掛載到 `/Volumes/<卷標>`
- 可同時掛載多顆 NTFS 硬碟（每個卷各自獨立的 FSKit volume 與 libntfs-3g 狀態）
- 簡單 UI：列出 NTFS 卷、手動卸載、以唯讀方式重新掛載（Remount Read-Only）、以讀寫方式重新掛載、手動掛載

非目標（YAGNI）：格式化 NTFS、修復（僅提供 DiskArbitration 要求的最小 fsck）、NTFS ACL/使用者對應、壓縮/加密檔案寫入、Kernel-offloaded I/O 效能最佳化。

## 2. 關鍵事實（研究結論）

- FSKit 擴充是 ExtensionKit 擴充（product type `com.apple.product-type.extensionkit-extension`），Info.plist 使用 `EXAppExtensionAttributes`，`EXExtensionPointIdentifier = com.apple.fskit.fsmodule`，入口點必須是 Swift 的 `UnaryFileSystemExtension`。
- 需要 entitlement `com.apple.developer.fskit.fsmodule` 與 App Sandbox。AMFI 要求該 entitlement 由 provisioning profile 授權，因此**必須以 Apple Developer Team 簽章**（ad-hoc 不會載入）。
- 使用者必須在「系統設定 > 一般 > 登入項目與延伸功能 > 檔案系統延伸功能」啟用本擴充。
- DiskArbitration 自動掛載條件：Info.plist 內 `FSMediaTypes` 宣告對應 `Content Hint`（`Windows_NTFS`、GPT Microsoft Basic Data GUID `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`、`Partitionless`），`FSProbeOrder` 必須小於系統內建 `ntfs.fs`（1000 / 2000 / 4000），擴充需實作 `FSManageableResourceMaintenanceOperations.startCheck`（支援 `-q`、`-y`、`-n`，需非同步完成），probe 必須回傳 `.usable`，且 `containerID` 必須等於 volume 的 `volumeID`。
- 使用者層級的手動掛載/卸載走 `diskutil mount [readOnly] <dev>` / `diskutil unmount <dev>`，經由 diskarbitrationd 呼叫 FSKit，不需要 root。`mount -F -t <FSShortName>` 需要 root，只作為備援。
- `FSShortName` 不可用 `ntfs`（會與內建 kext 衝突），本專案採 `ntfs3g`。
- `FSBlockDeviceResource` 的讀寫必須是磁區對齊；本專案統一使用直接 I/O（`readInto/writeFrom`），不混用 metadata 快取 API。

## 3. 架構

```
MacNTFS.xcodeproj（由 XcodeGen 的 project.yml 產生）
├── MacNTFS.app                （SwiftUI 選單列 App）
│   ├── DiskMonitor            DiskArbitration 監看 NTFS 卷出現/消失/描述變更
│   ├── MountController        diskutil mount / unmount / mount readOnly；備援 mount -F（需管理員）
│   ├── ExtensionStatus        FSClient 查詢擴充是否已安裝/啟用，引導使用者到系統設定
│   └── UI                     選單列 + 視窗：卷清單、掛載狀態、按鈕（卸載 / 唯讀重掛 / 讀寫重掛 / 掛載）
├── NTFSFSExtension.appex      （FSKit 擴充）
│   ├── NTFSFSExtension.swift  @main UnaryFileSystemExtension（Swift 入口，必要）
│   ├── NTFSFileSystem.m       FSUnaryFileSystem：probe / load / unload / startCheck
│   ├── NTFSVolume.m           FSVolume + Operations / OpenClose / ReadWrite / Xattr / PathConf
│   ├── NTFSItem.m             FSItem 子類：只存 MFT 編號與快取型別，不長期持有 ntfs_inode
│   ├── NTFSBlockDevice.m      libntfs-3g ntfs_device_operations → FSBlockDeviceResource（含磁區對齊 bounce）
│   └── ntfs_bridge.c/.h       純 C 橋接層：以 MFT 編號為單位封裝 libntfs-3g 操作
├── ntfs-bridge-test           （CLI 測試工具，用一般檔案模擬區塊裝置測試橋接層）
├── ThirdParty/ntfs-3g         Scripts/build-ntfs3g.sh 產生的 universal 靜態庫與標頭（LGPL）
└── Scripts/build-ntfs3g.sh    下載 ntfs-3g 2026.7.7、編譯 arm64+x86_64、lipo 合併
```

語言選擇：擴充核心用 Objective-C + C（與 libntfs-3g 的巨集、union、`ntfschar` 互通零摩擦），入口用 Swift（FSKit 硬性要求），App 用 SwiftUI。

## 4. 元件設計

### 4.1 ntfs_bridge（C）

對外只暴露不透明 `ntfsb_volume*` 與以 `uint64_t mft_no` 為身分的操作，每個操作內部 `ntfs_inode_open` → 操作 → `ntfs_inode_close`（與 ntfs-3g FUSE 驅動相同模式，避免同一 inode 重複開啟造成 MFT 記錄不一致）。

裝置抽象：呼叫端提供 `ntfsb_device_callbacks { pread, pwrite, sync, block_size, block_count, writable }`；橋接層把 libntfs-3g 的任意偏移讀寫轉成磁區對齊的讀-改-寫。ioctl 只回應 `DKIOCGETBLOCKSIZE`、`DKIOCGETBLOCKCOUNT`、`DKIOCGETPHYSICALBLOCKSIZE`。

主要 API：
- `ntfsb_probe(callbacks, out_info)`：讀 boot sector 驗證 `"NTFS    "`，取序號；唯讀掛載取得卷標後立即卸載
- `ntfsb_mount(callbacks, read_only, &err)` / `ntfsb_umount`：`NTFS_MNT_RECOVER | NTFS_MNT_MAY_RDONLY`，`ntfs_set_shown_files(sys=否, hidden=是, dot=否)`、`ntfs_volume_get_free_space`
- 不分大小寫但保留大小寫：**不用** `ntfs_set_ignore_case`（它會讓 `ntfs_readdir` 把檔名全部轉小寫），改為清除 `NVolCaseSensitive` 旗標並把 `vol->locase` 設成恆等表，查找走 IGNORE_CASE collation、列舉保留原始大小寫
- `ntfsb_parent`：以 `ntfs_dir_parent_inode` 取得父目錄，供 FSKit 的 parentID 使用
- `ntfsb_getattr`、`ntfsb_lookup`、`ntfsb_readdir`（cookie = ntfs_readdir 的 pos；回呼延遲一筆以取得 nextCookie）
- `ntfsb_read`、`ntfsb_write`、`ntfsb_truncate`
- `ntfsb_create`（檔案/目錄/符號連結）、`ntfsb_unlink`（`ntfs_delete`）、`ntfsb_rename`（`ntfs_link` + `ntfs_delete`，覆蓋目標時先刪除）、`ntfsb_link`
- `ntfsb_readlink`（reparse point 走 `ntfs_make_symlink`，Interix 走 INTX 資料）
- `ntfsb_set_times`、`ntfsb_set_flags`（READONLY/HIDDEN）
- xattr 對應到 NTFS 具名 `$DATA` 串流：`ntfsb_xattr_list/get/set/remove`
- `ntfsb_statfs`、`ntfsb_sync`、`ntfsb_volume_label`、`ntfsb_is_readonly`

### 4.2 NTFSFSExtension（Objective-C）

- **probeResource**：只接受 `FSBlockDeviceResource`；成功回 `.usable(name: 卷標, containerID: 由 NTFS 序號衍生的穩定 UUID)`；休眠（hiberfile）或非 NTFS 回 `.notRecognized` / `.recognized`。
- **loadResource**：建立 `NTFSVolume`（volumeID 與 probe 的 containerID 相同）；實際 libntfs-3g 掛載延後到 `activate`。
- **startCheck**：協定本身沒有 resource 參數，依 Apple 開源 msdosfs 的做法使用 `loadResource` 存下的最近一個 `FSBlockDeviceResource`；以 libntfs-3g 唯讀掛載驗證可用性，`-q`/`-n`/`-y` 皆為「可解析即通過」，休眠卷在非 `-q` 時回 EPERM；透過 `NSProgress` 與 `FSTask.didComplete` 非同步完成（同步完成會被 fskitd 判定為錯誤 27503）。
- **NTFSVolume**：
  - 所有 libntfs-3g 呼叫以每卷一把遞迴鎖序列化（libntfs-3g 非執行緒安全）。
  - `activate`：解析 `-o rdonly` 與 `requestedMountOptions`、`-u/-g/-m`，掛載；根目錄 item ID 固定為 `FSItemIDRootDirectory`，其他 item ID = `mft_no + 64`（避免與保留值 0/1/2 衝突）。
  - `enumerateDirectory`：`attributes == nil` 時包含 `.`/`..`，否則不包含；verifier 為每個目錄的修改世代計數。
  - 權限模型：不做 NTFS 使用者對應；uid/gid 取掛載選項，預設 99（unknown，等同 Finder「忽略此卷的擁有權」），mode 0777 減 umask；`FILE_ATTR_READONLY` 去掉寫入位元，`FILE_ATTR_HIDDEN` 對應 `UF_HIDDEN`。
  - 回報 caseFormat = insensitiveCasePreserving、maximumFileSize = 2^64-1、maximumNameLength = 255、maximumLinkCount = 1023、supportsHardLinks/SymbolicLinks/SparseFiles = YES、doesNotSupportSettingFilePermissions = YES。
  - `requestedMountOptions`（macOS 26.4+）以 `API_AVAILABLE` 手動實作存取子，舊系統只看 `-o rdonly`。
  - `synchronize` → `ntfsb_sync`；`deactivate`/`unmount` → 卸載。
- **NTFSBlockDevice**：以 `FSBlockDeviceResource` 的同步 `readInto:startingAt:length:error:` / `writeFrom:` 實作 callbacks。

### 4.3 MacNTFS.app（SwiftUI）

- `LSUIElement` 選單列 App，`MenuBarExtra` 顯示卷清單與狀態；另有設定/狀態視窗。
- `DiskMonitor`：`DARegisterDiskAppearedCallback`、`DiskDisappeared`、`DiskDescriptionChanged`，篩選 `DAMediaContent ∈ {Windows_NTFS, EBD0A0A2-…}` 或 `DAVolumeKind ∈ {ntfs, ntfs3g}`。
- 自動掛載：每個 NTFS 卷出現後
  1. 若 DA 已用本擴充掛載（`DAVolumeKind == ntfs3g`）→ 只更新 UI。
  2. 若尚未掛載 → `diskutil mount <dev>`（DA 會依 FSProbeOrder 選用本擴充）。
  3. 若被內建 kext 以唯讀掛載（`DAVolumeKind == ntfs`）→ 標示「系統唯讀掛載」，提供「改用 MacNTFS 掛載」按鈕：`diskutil unmount` 後執行備援 `mount -F -t ntfs3g`（AppleScript 管理員權限）。
  自動掛載可在設定關閉。
- 手動操作：卸載（`diskutil unmount`）、唯讀重掛（unmount → `diskutil mount readOnly`）、讀寫重掛（unmount → `diskutil mount`）。
- 擴充狀態：`FSClient.shared.installedExtensions` 檢查是否啟用，未啟用時顯示提示與「開啟系統設定」按鈕。

## 5. 錯誤處理

- libntfs-3g 以 errno 回報；橋接層一律回傳負 errno，ObjC 層轉成 `fs_errorForPOSIXError`。
- 掛載失敗（休眠、損毀、裝置唯讀）：`NTFS_MNT_MAY_RDONLY` 允許退回唯讀，並在 UI 顯示原因。
- 裝置 I/O 錯誤：callbacks 回傳 -1 並設定 errno=EIO，libntfs-3g 會將卷標記為需修復。
- diskutil 失敗：擷取 stderr 顯示於 UI。

### 4.4 Info.plist 探測順序

系統內建 `/System/Library/Filesystems/ntfs.fs` 的 `FSProbeOrder` 為 Windows_NTFS=1000、GPT Basic Data GUID=2000、Partitionless=4000；本擴充宣告 500 / 1500 / 3500 以取得優先權。`FSShortName = ntfs3g`。

## 6. 測試

- `ntfs-bridge-test` CLI：以 `mkntfs` 建立的 64 MB 映像檔為裝置，跑 mount → mkdir → create → write → read → rename → xattr → unlink → statfs → umount 的煙霧測試，並用 `ntfsls`/`ntfscat` 交叉驗證。（2026-09-07：66 項全部通過）
- `xcodebuild` 全部 target 編譯通過。（2026-09-07：`MacNTFS`、`ntfs-bridge-test` 兩個 scheme 皆 BUILD SUCCEEDED，未簽章）
- 端對端（需使用者操作）：簽章、放到 /Applications、啟用擴充、插入 NTFS 碟或 `hdiutil attach -nomount` 映像檔，觀察自動掛載；`log stream --predicate 'subsystem == "com.apple.FSKit"'` 除錯。

## 7. 已知限制與假設

- 假設使用者有 Apple Developer Team 可簽署受限 entitlement；`project.yml` 的 `DEVELOPMENT_TEAM` 需自行填入。
- Bundle ID 前綴採 `com.rookiestudio.MacNTFS`，可在 `project.yml` 修改。
- 不實作 Kernel-offloaded I/O；大檔案吞吐量受 XPC 往返限制。
- 不支援 NTFS 壓縮/加密檔案的寫入（讀取由 libntfs-3g 支援）。
- xattr 以具名串流實作，Windows 端會看到 `檔名:xattr名` 串流。
