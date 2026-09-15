//
//  bench.h
//  ntfs-bridge-test
//
//  橋接層效能量測。不需要掛載，直接對映像檔量 ntfs_bridge + libntfs-3g 這一段，
//  所以可以在不經過 FSKit 的情況下驗證橋接層的改動是否真的有效。
//

#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

/// 對映像檔執行效能量測。成功回傳 0，失敗回傳非 0。
/// 注意：會在映像檔內建立檔案，執行後映像檔內容會改變。
int bench_run(const char *image_path, uint32_t block_size);

#endif /* BENCH_H */
