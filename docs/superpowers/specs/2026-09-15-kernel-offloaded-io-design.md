# Kernel Offloaded I/O（KOIO）設計文件

日期：2026-09-15
狀態：橋接層已完成並通過測試；FSKit 層對唯讀與讀寫掛載都已啟用（第 13 節的死結經第 14 節診斷後無法重現）。
`ntfsb_koio_eligible` 實作簽章改為 `(const ntfsb_attr *, uint32_t cluster_size)`，
且空的常駐檔案視為合格（blockmap 時轉非常駐，因為 kernel 只在 lookup/create/getattr 取旗標）。

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
4. 立刻把 `init_size` 推進到最後一個交給 packer 的 extent 結尾（不超過 `offset+length`），不等 completeIO。不變式：`init_size` 涵蓋所有交給 kernel 的寫入範圍，所以第 2 步與慢路徑 `ntfs_attr_pwrite` 的補零只會碰到真正的洞（見第 15 節）。

## 6. completeIO 語意

- Write 成功：`data_size`／`init_size` 已在 blockmap 推進，這裡只是保險（取 max）並更新 mtime／ctime。
- Write 失敗（`status` 非 nil）：blockmap 已把該段算進 `init_size`，磁碟上卻可能是舊資料，必須改回零：
  - `offset+length >= init_size`（檔尾那段）：把 `init_size` 退回 `offset`，不再碰可能正在出錯的裝置。
  - 否則（中間那段，後面已有資料）：以 `ntfs_attr_pwrite` 把 `[offset, offset+length)` 覆寫成零。
- 已知取捨：磁碟上的 `init_size` 會比資料早一個非同步寫入的時間窗落地，若在 kernel 寫完前當機，該段會露出叢集上的舊內容而非零。libntfs-3g 自己的 pwrite 也是先改 init 再寫資料，這只是把既有的時間窗拉長。
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

## 13. 端對端發現：讀寫掛載的 kernel 死結（2026-09-15）

里程碑 1／2 端對端測試時，凡是對**讀寫掛載**的卷寫入（`cp` 一開檔即觸發）就會讓 kernel 的 lifs 層死結：

- 延伸功能程序全程閒置（所有執行緒停在 `mach_msg`／`__workq_kernreturn`），代表請求根本沒送達延伸功能。
- kernel 日誌在掛載後只到 `lifs_enable_lowspace_sync_write` 就停止，之後對該卷再無任何 lifs 訊息。
- 連 `diskutil unmount force` 都卡住；`kill -9` 延伸功能程序也無法解除，只能重開機。NTFS 本身與 libntfs-3g 中繼資料不受損（與既有的 `fskitd` 殘留警告一致）。

推測成因：新建檔案在第一次 `getattr` 之前，kernel 預設它可走 KOIO（因為卷宣告 `FSSupportsKernelOffloadedIO`），於是對 `blockmapFile(write)` 送出請求並持有該檔案範圍的 lifs 鎖等待回覆；而我們在 `ntfsb_blockmap`（寫入）內配置叢集與補零時，會同步呼叫 `FSBlockDeviceResource` 寫入——這條路又回到 lifs，需要 kernel 正持有的鎖，形成 kernel↔延伸功能的鎖循環。

影響評估：這是設計裡「在 `blockmapFile` 內做空間配置」的根本問題，不是實作 bug——橋接層 `ntfsb_blockmap`／`ntfsb_complete_io`／`truncate_solid` 的單元測試（含假 kernel 直接對映像檔 I/O）全數通過。純讀取路徑不配置空間，理論上不受影響，但尚未取得乾淨的讀取端對端佐證（測試都先卡在寫入）。

目前處置：`FSSupportsKernelOffloadedIO` 設回 `false`（commit 48666ea），保護使用者平常讀寫掛載的實體碟。全部橋接層程式碼與測試保留。

後續方向（待在可頻繁重開機的環境驗證）：
1. 把空間配置移出 `blockmapFile`——kernel 呼叫 blockmap 時所有叢集都應已配置好。做法是實作 `FSVolumePreallocateOperations`（第 10 節），或在 create／首次寫入的慢路徑先把檔案配置到位，讓 blockmap 純粹回報既有 runlist、不做任何裝置寫入。
2. 若步驟 1 後 blockmap 仍需補零（`[init_size, offset)`），改由 `completeIO` 或慢路徑處理，確保 blockmap 期間零裝置 I/O。
3. 先只在唯讀掛載開 KOIO 取得讀取路徑的乾淨佐證，再回頭處理寫入。

## 14. 診斷 spike 結果：乾淨開機後讀寫 KOIO 無法重現死結（2026-09-15）

分支 `spike/koio-diag`（commit f4c23d2）在讀寫掛載也啟用 KOIO，並加上持久化 `KOIO-DIAG` 日誌。重開機（讓 pkd／fskit_agent／fskitd 重新掃描延伸功能）後的端對端結果：

- **開機自動掛載**：macOS 在開機時就用這個 spike 版本把兩顆實體 NTFS 碟讀寫掛載，kernel 隨即透過 KOIO 建立 `.fseventsd/fseventsd-uuid`（blockmap write → 配置叢集 → completeIO → getattr 回報 size=36 alloc=4096），一切正常，沒有死結。
- **測試映像檔（256 MB，`diskutil mount` 不需 sudo 即走 ntfs3g）**依序通過：讀取 1 MB（md5 相符）、`cp` 小檔、`cp` 1 MB、`cp` 64 MB（kernel 以 1 MB 為單位呼叫 blockmap 64 次，每次 0.01～0.03 ms）、中段覆寫（read-modify-write，completeIO op=0）、追加、跨 EOF 稀疏寫入、截短後重寫、Finder 複製（copyfile 路徑）、3 個並行 40 MB 寫入 + 並行讀取 + 建 50 檔目錄樹再 `rm -rf`。卸載重掛後所有內容都以 md5／逐位元組比對驗證正確。
- **第 13 節的假設被推翻**：跨 EOF 稀疏寫入時，`blockmapFile(write)` 內確實同步做了一次 98304 位元組的補零 `pwrite`（`[init_size=0, 98304)`），kernel 正持有該檔案的 lifs 鎖等 blockmap 回覆——這正是原本推測會形成鎖循環的情境，實際上沒有死結。整輪測試 257 次 blockmap、256 次 completeIO，慢路徑寫入 0 次。
- 也觀察到 kernel 在 `lifs_enable_lowspace_sync_write` 之後仍正常運作，所以第 13 節「日誌停在此處」不是該訊息本身的問題。

**結論**：讀寫掛載的 KOIO 在乾淨的系統狀態下可正常運作；第 13 節的死結最可能是當時反覆 killall／重新註冊延伸功能後 fskitd／kernel lifs 的殘留狀態造成（同一段時間也出現過 `Failed to store the mount point in settings file` 與「No extension with fsShortName ntfs3g」），而不是設計缺陷。仍無法百分之百排除某個未測到的路徑，因此正式啟用後應保留一段觀察期。

證據：`~/koio-diag/log.txt`（測試映像檔全程）、`~/koio-diag/boot-physical-disks-log.txt`（開機自動掛載實體碟）。

## 15. 資料損毀：分段寫入時前段被補零（2026-10-16）

**現象**：用 Finder 把「1.5 MB 的 `.mdx` + 兩個 3.3 GB 的 `.mdd`」資料夾複製到 ntfs3g 卷宗，`.mdx` 大小正確，但 `[0, 0x160000)` 全是零、只有尾端 72 KB 正確，兩次結果完全相同。單獨複製該檔 6 次（cp／Finder）都正確；同一資料夾複製到 APFS 也正確，排除讀取端。Finder 不會回報任何錯誤。

**根本原因**：舊設計只在 completeIO 推進 `init_size`。kernel 在寫回壓力下把同一個檔案拆成 `[0, A)`、`[A, B)` 兩次 blockmap；前段已直接寫進磁碟、completeIO 還沒回來時，後段 blockmap 的第 2 步看到 `init_size` 仍是 0，便把 `[0, A)` 當成洞補零，蓋掉 kernel 剛寫的資料。之後兩次 completeIO 把 `init_size` 推到 EOF，零就留了下來。慢路徑的 `ntfs_attr_pwrite` 在 `pos > init_size` 時也會補零（`attrib.c:2059`），有同樣的風險。

**修正**：blockmap 交出寫入範圍時立刻推進 `init_size`（第 5 節第 4 步），失敗的寫入改由 completeIO 退回或覆寫成零（第 6 節）。`ntfs-bridge-test` 的 `test_write_interleaved` 以假 kernel 重現此順序：修正前「前段內容沒有被補零蓋掉」失敗，修正後全數通過；另含檔尾失敗與中段失敗兩個案例。

**影響範圍**：f134a2a（2026-09-15 讀寫掛載啟用 KOIO）之後經由 KOIO 寫入的檔案都可能受影響，且沒有任何錯誤提示，應以雜湊比對複製結果。

**同一天一併修正**：`maximumFileSize` 回傳 `UINT64_MAX` 時，系統換算出的 `_PC_FILESIZEBITS` 是 0，Foundation 便把卷宗上限當成 2 GB，Finder 會以「其大小對於卷宗格式而言過於龐大」拒絕複製大於 2 GB 的檔案；改回傳 `INT64_MAX` 後 `FILESIZEBITS` 為 64。這個限制原本意外擋住了大型資料夾的複製，修掉後才讓上面的損毀浮現。
