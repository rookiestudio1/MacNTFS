//
//  file_device.h
//  ntfs-bridge-test
//
//  以一般檔案（NTFS 映像檔）模擬區塊裝置，供正確性測試與效能量測共用。
//  除了轉呼叫 pread/pwrite 之外，還會累計呼叫次數與位元組數：
//  這是判斷橋接層改動是否生效最可靠的指標，因為它不受底層 SSD 快取影響。
//

#ifndef FILE_DEVICE_H
#define FILE_DEVICE_H

#include <stdint.h>
#include <stdio.h>
#include "ntfs_bridge.h"

/// 裝置 I/O 統計
typedef struct file_device_stats {
  uint64_t read_calls;
  uint64_t read_bytes;
  uint64_t write_calls;
  uint64_t write_bytes;
  uint64_t sync_calls;
} file_device_stats;

/// 用檔案描述子模擬的裝置
typedef struct file_device {
  int fd;
  int64_t size;
  file_device_stats stats;
  /// 非 NULL 時把每次 I/O 記成一行 "r|w <offset> <length>"，用來歸因裝置流量
  FILE *trace;
} file_device;

/// 開始 / 停止記錄 I/O 軌跡。path 為 NULL 表示停止。
int file_device_trace(file_device *d, const char *path);

/// 開啟映像檔。成功回傳 0，失敗回傳 -1 並設定 errno。
int file_device_open(file_device *d, const char *path);

/// 關閉映像檔
void file_device_close(file_device *d);

/// 依映像檔大小與 block_size 填好 ntfsb_device_callbacks
ntfsb_device_callbacks file_device_callbacks(file_device *d, uint32_t block_size);

/// 歸零統計
void file_device_reset_stats(file_device *d);

#endif /* FILE_DEVICE_H */
