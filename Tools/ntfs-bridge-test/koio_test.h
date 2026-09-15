//
//  koio_test.h
//  ntfs-bridge-test
//

#ifndef KOIO_TEST_H
#define KOIO_TEST_H

#include "file_device.h"
#include "ntfs_bridge.h"

/// Kernel offloaded I/O 的橋接層測試：用 file_device 直接讀寫映像檔扮演 kernel。
/// 回傳失敗項目數。
int koio_test_run(ntfsb_volume *vol, file_device *dev);

/// 卸載重掛後檢查 KOIO 寫入的檔案內容與大小仍正確。回傳失敗項目數。
int koio_test_verify_after_remount(ntfsb_volume *vol);

#endif /* KOIO_TEST_H */
