//
//  ntfs_blockcache.h
//  NTFSFSExtension
//
//  夾在橋接層與裝置回呼之間的區塊快取。
//
//  目的是減少「裝置 I/O 的呼叫次數」，而不是位元組數：在真實的 FSKit 路徑上，
//  每一次 pread/pwrite 都是一趟到 kernel 的 IPC，而 NTFS 的中繼資料
//  （MFT record、$MFT 的 $BITMAP、目錄索引）會被反覆讀寫同樣的幾個區塊。
//
//  策略：
//  - 小請求走快取；單次 >= NTFSB_BCACHE_BYPASS 的大請求直接走裝置，
//    並使重疊的行失效。循序資料讀寫屬於後者，不會把中繼資料擠出快取。
//  - 寫入為 write-back，在 flush（也就是裝置 sync）時才落盤。
//    檔案內容因為走大請求繞過路徑，永遠是直寫，不受影響。
//
//  本模組不處理對齊：offset 與 count 必須是 block_size 的整數倍。
//  也不是執行緒安全的，呼叫端必須自行序列化。
//

#ifndef NTFS_BLOCKCACHE_H
#define NTFS_BLOCKCACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "ntfs_bridge.h"

/// 快取行大小（一個叢集＝四筆 MFT record）
#define NTFSB_BCACHE_LINE   4096
/// 快取總量
#define NTFSB_BCACHE_BYTES  (4 * 1024 * 1024)
/// 單次請求達到這個大小就繞過快取
#define NTFSB_BCACHE_BYPASS (64 * 1024)

typedef struct ntfsb_bcache ntfsb_bcache;

/// 建立快取。記憶體不足時會退化為純轉呼叫（仍回傳有效物件）。
/// 只有連結構本身都配置不出來才回傳 NULL。
ntfsb_bcache *ntfsb_bcache_create(const ntfsb_device_callbacks *cb, int64_t device_size);

/// 釋放（不會自動 flush，呼叫端要先 flush）
void ntfsb_bcache_free(ntfsb_bcache *c);

/// 設為唯讀：拒絕寫入，也不會產生髒行
void ntfsb_bcache_set_readonly(ntfsb_bcache *c, bool readonly);

/// 讀取。offset 與 count 必須對齊 block_size。
/// 回傳實際讀取位元組數；失敗回傳 -1 並設定 errno。
int64_t ntfsb_bcache_pread(ntfsb_bcache *c, void *buf, int64_t count, int64_t offset);

/// 寫入。offset 與 count 必須對齊 block_size。
/// 回傳實際寫入位元組數；失敗回傳 -1 並設定 errno。
int64_t ntfsb_bcache_pwrite(ntfsb_bcache *c, const void *buf, int64_t count, int64_t offset);

/// 把所有髒行寫回裝置。成功回傳 0，失敗回傳 -1 並設定 errno。
int ntfsb_bcache_flush(ntfsb_bcache *c);

/// 把 [offset, offset+count) 涵蓋的髒行寫回，並讓範圍內所有行（髒或乾淨）失效。
/// 供 kernel offloaded I/O 使用：交給 kernel 直接 I/O 的範圍，快取不能再持有任何副本。
/// offset 與 count 必須對齊 block_size。成功回傳 0，失敗回傳 -1 並設定 errno。
int ntfsb_bcache_flush_range(ntfsb_bcache *c, int64_t offset, int64_t count);

#endif /* NTFS_BLOCKCACHE_H */
