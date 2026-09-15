# Kernel Offloaded I/O（KOIO）設計文件

日期：2026-09-15
狀態：設計已審閱，待撰寫實作計畫

## 1. 目標

讓一般檔案的資料 I/O 不再經由 XPC 往返與資料複製，而是由延伸功能提供邏輯→實體 extent 對應，交給 kernel 直接對 `FSBlockDeviceResource` 做 I/O（FSKit 的 `FSVolumeKernelOffloadedIOOperations`）。

範圍：
- 讀寫掛載下，一般、非常駐、非壓縮／加密／sparse 的檔案資料讀寫全部走 KOIO。
- 不合格的檔案沿用既有 `FSVolumeReadWriteOperations` 慢路徑，不改動。
- 同時完成效能待辦裡的「縮短大請求持鎖時間」：blockmap／completeIO 在鎖內只做中繼資料工作，資料 I/O 由 kernel 在鎖外進行。

非目標（YAGNI）：
- `FSVolumePreallocateOperations`（見第 9 節的未來接法）。
- 在 `createFileNamed:…packer:`／`lookupItemNamed:…packer:` 機會性提供 extent。
- sparse／壓縮／加密檔案走 KOIO。
- KOIO 讀取更新 atime。

## 2. 關鍵事實（來自 SDK 標頭與 libntfs-3g 原始碼）

- `FSVolume` 可同時遵循 `FSVolumeReadWriteOperations` 與 `FSVolumeKernelOffloadedIOOperations`；每個檔案由 `FSItemAttributes.inhibitKernelOffloadedIO` 決定走哪條路（`FSVolume.h` 850–855 行）。
- `blockmapFile:` 的 `operationID` 非 0 表示是一次 I/O 的開頭，之後會有同 ID 的 `completeIOForFile:`；為 0 表示 `fcntl(F_LOG2PHYS)`，不會有 completeIO。kernel 會保留映射並隨時可能對該範圍 I/O。packer 滿時 FSKit 會用同一 ID 再呼叫一次拿剩下的。
- `FSExtentPacker` 以位元組為單位：`logicalOffset`（檔內）、`physicalOffset`（裝置內）、`length`；型別 `Data` 或 `ZeroFill`。實體位移與現有裝置回呼同一座標系（分割區相對）。
- `completeIOForFile:` 的責任是更新大小與時間戳；`FSCompleteIOFlagsAsync` 表示中繼資料可非同步寫回。
- libntfs-3g 的 `ntfs_attr_truncate`（`HOLES_OK`）在 NTFS 3+ 拉長 `$DATA` 時**留洞不配置**（`attrib.c` `ntfs_non_resident_attr_expand_i`），並在 `ntfs_attr_update_mapping_pairs` 把屬性標成 `ATTR_IS_SPARSE`；`ntfs_attr_truncate_solid`（`HOLES_NO`）才會真的配置叢集。
- `ntfs_attr_pwrite` 在 `pos > initialized_size` 時會自行把 `[initialized_size, pos)` 補零並推進 `initialized_size`。
- NTFS 的 `initialized_size` 之後、`data_size` 之前的範圍在磁碟上是垃圾，必須以零呈現。
- `ntfs_blockcache` 是 write-back、4 MB／4096 位元組行、≥ 64 KB 的請求繞過並使重疊行失效；libntfs-3g 與 blockcache 都不是執行緒安全，由 `NTFSFairLock` 序列化。

## 3. 架構與資料流

```
kernel ──(blockmap / completeIO：只有 extent 與中繼資料)──▶ NTFSVolume ──▶ ntfsb_blockmap / ntfsb_complete_io ──▶ libntfs-3g runlist
  │                                                              │ 全部在 NTFSFairLock 內
  └──(資料 I/O 直接對 FSBlockDeviceResource)──▶ 磁碟             └──▶ ntfsb_bcache_flush_range（回覆前）
inhibit 的檔案：kernel ──(read / write，資料經 XPC)──▶ 既有 readFromFile / writeContents 慢路徑（不變）
```

變更點：
- `project.yml`／`Info.plist`：`FSSupportsKernelOffloadedIO: true`。
- `NTFSVolume`：加入 `FSVolumeKernelOffloadedIOOperations`，實作 `blockmapFile:`、`completeIOForFile:`、`createFileNamed:…packer:`、`lookupItemNamed:…packer:`（後兩者轉呼叫既有 create／lookup、忽略 packer）；`getattr` 不再寫死 `inhibitKernelOffloadedIO = YES`。
- `ntfs_bridge`：新增 `ntfsb_koio_eligible`、`ntfsb_blockmap`、`ntfsb_complete_io`；`ntfsb_attr` 增加 `resident`、`init_size`；`ntfsb_truncate` 拉長改用 `ntfs_attr_truncate_solid`。
- `ntfs_blockcache`：新增 `ntfsb_bcache_flush_range`。
- `Tools/ntfs-bridge-test`：新增 `koio_test.c` 與 bench 項目。

## 4. 合格判定

在 `getattr` 時計算，寫入 `inhibitKernelOffloadedIO`。以下任一成立即 inhibit，其餘一律走 KOIO：

1. `$DATA` 為常駐——資料在 MFT record 內，沒有叢集可映射。
2. 壓縮或加密——慢路徑負責解壓／拒絕。
3. sparse——runlist 有洞，寫入需補洞，本次不處理。
4. `init_size < data_size` **且** `init_size` 不在叢集邊界——半滿叢集的尾巴在磁碟上是垃圾，kernel 讀不出零。只擋這種罕見情況；init 對齊的 Windows 預配置檔仍走 KOIO，超過 init 的範圍以 `ZeroFill` 回覆。

規則實作在橋接層 `ntfsb_koio_eligible(vol, attr)`，測試工具與 NTFSVolume 用同一份。

狀態轉換（常駐→非常駐、init 追上 data_size）由 kernel 下次取屬性時自然切換；切換前多走幾次慢路徑只是效能問題，不影響正確性。反向轉換（非常駐→常駐）libntfs-3g 不會自動做；壓縮旗標本專案不提供設定；sparse 旗標只會由 `ntfs_attr_truncate` 留洞產生，因此 `ntfsb_truncate` 改用 `truncate_solid` 後，走 KOIO 的檔案不會在我們手上變 sparse。

## 5. blockmap 語意

共通：
- 只接受一般檔案；目錄回 EISDIR、其他型別回 EINVAL（FSKit 理論上不會送，防禦用）。
- 有 handle 就借用 handle 的 inode／attr，沒有就臨時開（沿用 `ntfsb_open_inode`／`ntfsb_close_inode`），呼叫端不必區分。
- 用 `ntfs_attr_map_whole_runlist` 取得 runlist，依 `[offset, offset+length)` 與叢集對齊後的範圍交集切出 extent：`logical = vcn × cluster_size`、`physical = lcn × cluster_size`。
- 每打包一個 `Data` extent，先對其實體範圍呼叫 `ntfsb_bcache_flush_range`，再交給回呼。
- 回呼回傳非 0 表示 packer 已滿，停止打包並回 0（不算錯誤）；橋接層無狀態，FSKit 重叫時從新的 offset 接續。
- `operationID == 0`（`F_LOG2PHYS`）與非 0 做法相同。

Read：
- `[offset, offset+length) ∩ [0, ceil(init_size, cluster))` 依 runlist 打包 `Data`；runlist 內若意外碰到 `LCN_HOLE` 打包 `ZeroFill`。
- 超過 init 的部分打包成一個 `ZeroFill`，長度到 `offset+length`。

Write（唯讀掛載回 EROFS）：
1. `offset+length > data_size` → `ntfs_attr_truncate_solid(na, offset+length)`，配置叢集並設新 `data_size`（失敗回 ENOSPC）。
2. `offset > init_size` → 以 ≥ 64 KB 的零緩衝分段 `ntfs_attr_pwrite` 把 `[init_size, offset)` 補零（順便把 init 推到 `offset`）。最後一段可能 < 64 KB 進入 blockcache，由第 3 步的 flush_range 落盤。
3. 依 runlist 打包 `[offset, offset+length)` 的 `Data` extent。

## 6. completeIO 語意

- Write 成功：`data_size = max(data_size, offset+length)`（通常 blockmap 已設）、`init_size = max(init_size, min(offset+length, data_size))`、更新 mtime／ctime。
- Write 失敗（`status` 非 nil）：不動 `init_size`。`data_size` 已在 blockmap 拉長，但 init 未推進，所以慢路徑（libntfs-3g）與 KOIO（`ZeroFill`）讀到的都是零，兩邊一致。
- Read：不做事（不更新 atime）。
- 中繼資料寫回：
  - 有 handle → 設 `meta_dirty`，交給既有定時 flush；`FSCompleteIOFlagsAsync` 未設時額外立即 `ntfsb_file_flush`。
  - 無 handle（close 後 UBC 回寫、mmap）→ 臨時開 inode、改完即關，關閉時寫回 MFT record。
- 失敗只可能是 MFT 寫回失敗，回 EIO；kernel 端資料已在磁碟上。

## 7. 區塊快取一致性

`ntfsb_bcache_flush_range(c, offset, count)`：把範圍內髒行寫回，然後讓範圍內所有行（髒或乾淨）失效。快取只有 1024 行，掃一遍即可。

blockmap 對每個 `Data` extent 呼叫一次，封住三種情境：
1. 慢路徑寫進快取的小塊資料（常駐→非常駐轉換期、補零的最後一段）尚未落盤，kernel 直接讀磁碟會讀到舊值。
2. 叢集曾屬於中繼資料、釋放後配給檔案：舊髒行若在 kernel 寫完之後才寫回會蓋掉檔案內容。
3. 快取留著該叢集的乾淨舊行，kernel 寫過之後 libntfs-3g 再讀會拿到舊值。

不變條件：**交出去的叢集，橋接層不再經快取碰它。** 由合格規則保證——libntfs-3g 對 KOIO 檔案只會在 getattr、xattr（具名串流是另一個屬性）、rename、unlink 動到，都不讀 `$DATA` 的叢集。

## 8. 介面

### 8.1 `ntfs_bridge.h`

```c
/// 一段邏輯→實體對應。zero_fill 為 true 時 physical 無意義。
typedef struct ntfsb_extent {
  int64_t logical;   ///< 檔案內位移（位元組）
  int64_t physical;  ///< 裝置位移（位元組）
  int64_t length;
  bool zero_fill;
} ntfsb_extent;

/// 回傳非 0 表示 packer 已滿，停止打包（不算錯誤）
typedef int (*ntfsb_extent_cb)(void *context, const ntfsb_extent *extent);

/// 依第 4 節規則判定檔案是否可走 KOIO
bool ntfsb_koio_eligible(const ntfsb_volume *vol, const ntfsb_attr *attr);

/// 第 5 節。回傳 0 或負 errno。
int ntfsb_blockmap(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, int64_t length,
                   bool for_write, ntfsb_extent_cb cb, void *context);

/// 第 6 節。defer_meta 只在有 handle 時有效。回傳 0 或負 errno。
int ntfsb_complete_io(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, int64_t length,
                      bool for_write, bool io_failed, bool defer_meta);
```

`ntfsb_attr` 增加：
```c
bool resident;       ///< $DATA 為常駐
uint64_t init_size;  ///< initialized_size
```

`ntfsb_truncate` 行為變更：對非壓縮、非 sparse 的檔案拉長時改用 `ntfs_attr_truncate_solid`（真的配置叢集，與 Windows `SetEndOfFile` 一致）；縮短與其他情況不變。

### 8.2 `ntfs_blockcache.h`

```c
/// 把範圍內髒行寫回並讓範圍內所有行失效。offset 與 count 必須對齊 block_size。
/// 成功回傳 0，失敗回傳 -1 並設定 errno。
int ntfsb_bcache_flush_range(ntfsb_bcache *c, int64_t offset, int64_t count);
```

### 8.3 `NTFSVolume`

- `blockmapFile:offset:length:flags:operationID:packer:replyHandler:`：取 `NTFSItem.mftNo`，鎖內 `ntfsb_blockmap`；回呼把 `ntfsb_extent` 轉成 `packExtentWithResource:type:logicalOffset:physicalOffset:length:`（resource 為 `NTFSBlockDevice.resource`），packer 回 NO 則回呼回 1。
- `completeIOForFile:offset:length:status:flags:operationID:replyHandler:`：鎖內 `ntfsb_complete_io(..., status != nil, flags & FSCompleteIOFlagsAsync)`。
- `getattr`：`inhibitKernelOffloadedIO = !ntfsb_koio_eligible(_vol, &a)`。
- `createFileNamed:…packer:`／`lookupItemNamed:…packer:`：轉呼叫既有方法。

## 9. 錯誤處理

- blockmap：目錄 EISDIR、非一般檔案 EINVAL、唯讀掛載的 Write EROFS、配置失敗 ENOSPC、runlist 映射失敗 EIO。
- blockmap 中途失敗（例如補零到一半 ENOSPC）：已配置的叢集與已推進的 init 保留，回錯誤，不回滾——與 libntfs-3g `pwrite` 語意相同，狀態仍一致（init 只涵蓋已補零部分）。
- completeIO：MFT 寫回失敗回 EIO。

## 10. 未來：Preallocate

`FSVolumePreallocateOperations` 的 `flags` 目前一律含 `FromEOF`，需要「從 EOF 起配置叢集但不改 `data_size`」。libntfs-3g 沒有公開這種函式（`ntfs_non_resident_attr_expand` 為 static），接法是新增橋接函式：`ntfs_attr_truncate_solid` 到目標大小後，直接改屬性記錄把 `data_size` 設回原值（`allocated_size` 保留）。實作時要一併提供帶 packer 的 `preallocateSpaceForFile:`。

## 11. 測試

### 11.1 橋接層（`Tools/ntfs-bridge-test/koio_test.c`，`file_device` 直接對映像檔 pread／pwrite 扮演 kernel）

1. 合格規則：100 B 常駐檔 → 不合格；寫到 1 MB → 合格；`ntfs_attr_truncate` 留洞的 sparse 檔 → 不合格；init 未對齊 → 不合格。
2. Read blockmap：extent 覆蓋整個範圍且不重疊；實體位移處的原始資料 == `ntfsb_read` 讀到的；超過 init 的部分是 `ZeroFill`。
3. Write blockmap：1 MB 檔案在 offset 3 MB 寫 1 MB → `data_size` 4 MB、`[1 MB, 3 MB)` 補零；假 kernel 寫入圖樣 → `complete_io` → 卸載重掛 → 三段各自正確；`io_failed` 版本讀回零。
4. 快取一致性：慢路徑寫 8 KB（進 write-back 快取）→ blockmap → 直接 pread 映像檔必須已是新資料；反向：假 kernel 寫入後 `ntfsb_read` 讀到新資料。
5. packer 滿：回呼在第 N 個 extent 回 1，用接續 offset 再叫一次能銜接且不重疊。
6. 無 handle 路徑：以上在未 `ntfsb_file_open` 下也跑一遍。
7. bench 新項目：KOIO 循序讀寫的 blockmap 呼叫次數與裝置 I/O 次數（期望 handle 存在時 blockmap 本身零裝置讀）。

### 11.2 端對端（手動，實體碟或 hdiutil 映像檔）

分兩個里程碑，把 FSKit 文件沒寫清楚的行為在最便宜的階段驗證：
- **里程碑 1**：只有唯讀掛載時放行 KOIO（讀寫掛載暫時全 inhibit）。確認 plist、協定、blockmap 被呼叫、`F_LOG2PHYS`、大檔 md5 相同。
- **里程碑 2**：開放讀寫。驗證 `cp` 大檔來回 md5、`dd` 跳位寫入（補零）、`ftruncate` 後循序寫、mmap 讀寫、拷貝中 `ls -l` 延遲、Windows 建立的預配置檔讀取。

效能：實體碟上 `dd` 讀寫吞吐與延伸功能程序 CPU 用量前後對照。預期收益主要是省掉每次請求的 XPC 往返與資料複製，表現為 CPU 下降與延遲改善；真實媒體 0.1~1 GB/s，吞吐未必大幅提升。

## 12. 風險

- FSKit KOIO 文件簡略：kernel 端 extent 快取壽命、`ZeroFill` 範圍被寫入後是否重取映射、Write blockmap 是否會超過 EOF，只能靠里程碑 1／2 實測。若發現 `ZeroFill` 範圍被寫入後 kernel 不重取映射，備案是 Write blockmap 一律先把 `[init_size, offset+length)` 補零到磁碟。
- `truncate_solid` 改變既有慢路徑的拉長行為（真的占用空間），要在 README 記下。
- 端對端測試前必須先卸載所有由本延伸功能掛載的實體碟（見專案記憶），否則 `fskitd` 會留下殘留狀態。
