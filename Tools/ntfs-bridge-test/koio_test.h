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

#endif /* KOIO_TEST_H */
