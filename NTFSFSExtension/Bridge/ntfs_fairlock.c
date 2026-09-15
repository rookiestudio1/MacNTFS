//
//  ntfs_fairlock.c
//  NTFSFSExtension
//

#include "ntfs_fairlock.h"

int ntfsb_fairlock_init(ntfsb_fairlock *l) {
  if (!l) {
    return -1;
  }
  int rc = pthread_mutex_init(&l->mutex, NULL);
  if (rc) {
    return rc;
  }
  rc = pthread_cond_init(&l->cond, NULL);
  if (rc) {
    pthread_mutex_destroy(&l->mutex);
    return rc;
  }
  l->next_ticket = 0;
  l->now_serving = 0;
  l->owned = false;
  l->depth = 0;
  return 0;
}

void ntfsb_fairlock_destroy(ntfsb_fairlock *l) {
  if (!l) {
    return;
  }
  pthread_cond_destroy(&l->cond);
  pthread_mutex_destroy(&l->mutex);
}

void ntfsb_fairlock_lock(ntfsb_fairlock *l) {
  pthread_mutex_lock(&l->mutex);
  // 遞迴取得不領新號碼，否則會等到自己放開才輪到自己（死結）
  if (l->owned && pthread_equal(l->owner, pthread_self())) {
    l->depth++;
    pthread_mutex_unlock(&l->mutex);
    return;
  }
  uint64_t ticket = l->next_ticket++;
  while (l->owned || ticket != l->now_serving) {
    pthread_cond_wait(&l->cond, &l->mutex);
  }
  l->owned = true;
  l->owner = pthread_self();
  l->depth = 1;
  pthread_mutex_unlock(&l->mutex);
}

void ntfsb_fairlock_unlock(ntfsb_fairlock *l) {
  pthread_mutex_lock(&l->mutex);
  if (!l->owned || !pthread_equal(l->owner, pthread_self())) {
    // 不是持有者卻呼叫 unlock：程式錯誤，直接忽略而不是讓狀態壞掉
    pthread_mutex_unlock(&l->mutex);
    return;
  }
  if (--l->depth > 0) {
    pthread_mutex_unlock(&l->mutex);
    return;
  }
  l->owned = false;
  l->now_serving++;
  // 只有持有下一個號碼的人會通過 while 條件，但不知道是哪一條執行緒，只能全體喚醒。
  // 等待者多時這是 O(n) 次喚醒；實測 8 執行緒、每秒十幾萬次交接仍可接受，
  // 而真實負載的交接率約在每秒數千次（一次 FSKit 操作一次），差兩個數量級。
  // 若日後成為瓶頸，可改成依號碼分槽的多個 condvar 以縮小喚醒範圍。
  pthread_cond_broadcast(&l->cond);
  pthread_mutex_unlock(&l->mutex);
}
