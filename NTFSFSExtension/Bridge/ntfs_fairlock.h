//
//  ntfs_fairlock.h
//  NTFSFSExtension
//
//  先進先出的遞迴鎖。
//
//  NSRecursiveLock 與 pthread_mutex 在 Darwin 上都不保證取得順序，持續取鎖的
//  一方可以無限期餓死另一方。實測（bench 的「中繼資料延遲」）顯示，大檔讀取
//  進行中時 getattr 的 p99 會到 286 ms、最大 1.54 秒——也就是「複製檔案時
//  Finder 卡住」。
//
//  這把鎖用號碼牌保證依請求順序取得，讓後到的操作最多等一個進行中的操作。
//  支援同一執行緒遞迴取得。
//

#ifndef NTFS_FAIRLOCK_H
#define NTFS_FAIRLOCK_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct ntfsb_fairlock {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  uint64_t next_ticket;   ///< 下一個要發出的號碼
  uint64_t now_serving;   ///< 目前輪到的號碼
  pthread_t owner;
  bool owned;
  uint32_t depth;         ///< 遞迴深度
} ntfsb_fairlock;

/// 初始化。成功回傳 0。
int ntfsb_fairlock_init(ntfsb_fairlock *l);

/// 銷毀（呼叫時不可有人持有或等待）
void ntfsb_fairlock_destroy(ntfsb_fairlock *l);

/// 取得。同一執行緒可重複取得，需對應次數的 unlock。
void ntfsb_fairlock_lock(ntfsb_fairlock *l);

/// 釋放
void ntfsb_fairlock_unlock(ntfsb_fairlock *l);

#endif /* NTFS_FAIRLOCK_H */
