//
//  readdir_test.h
//  ntfs-bridge-test
//

#ifndef READDIR_TEST_H
#define READDIR_TEST_H

#include <stdint.h>
#include "ntfs_bridge.h"

/// 大目錄的分段列舉測試。回傳失敗項目數。
int readdir_test_run(ntfsb_volume *vol, uint64_t root_mft);

#endif /* READDIR_TEST_H */
