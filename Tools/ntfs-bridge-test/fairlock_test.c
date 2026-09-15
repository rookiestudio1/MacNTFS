//
//  fairlock_test.c
//  ntfs-bridge-test
//

#include "fairlock_test.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "ntfs_fairlock.h"

static double flt_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// MARK: - 互斥

#define FLT_THREADS 8
#define FLT_ROUNDS  20000

typedef struct {
  ntfsb_fairlock *lock;
  volatile long *counter;
} flt_mutex_ctx;

static void *flt_mutex_thread(void *arg) {
  flt_mutex_ctx *c = arg;
  for (int i = 0; i < FLT_ROUNDS; i++) {
    ntfsb_fairlock_lock(c->lock);
    // 刻意用非原子的讀-改-寫，沒有互斥就會掉更新
    long v = *c->counter;
    *c->counter = v + 1;
    ntfsb_fairlock_unlock(c->lock);
  }
  return NULL;
}

// MARK: - 飢餓

typedef struct {
  ntfsb_fairlock *lock;
  volatile int stop;
  long cycles;
} flt_greedy_ctx;

static void *flt_greedy_thread(void *arg) {
  flt_greedy_ctx *c = arg;
  while (!c->stop) {
    ntfsb_fairlock_lock(c->lock);
    c->cycles++;
    ntfsb_fairlock_unlock(c->lock);
  }
  return NULL;
}

// MARK: -

int fairlock_test_run(void) {
  int failures = 0;
  ntfsb_fairlock lock;
  if (ntfsb_fairlock_init(&lock) != 0) {
    fprintf(stderr, "失敗：FIFO 鎖初始化\n");
    return 1;
  }

  // ---- 互斥 ----
  {
    volatile long counter = 0;
    flt_mutex_ctx ctx = { .lock = &lock, .counter = &counter };
    pthread_t th[FLT_THREADS];
    for (int i = 0; i < FLT_THREADS; i++) {
      pthread_create(&th[i], NULL, flt_mutex_thread, &ctx);
    }
    for (int i = 0; i < FLT_THREADS; i++) {
      pthread_join(th[i], NULL);
    }
    long expect = (long)FLT_THREADS * FLT_ROUNDS;
    if (counter != expect) {
      fprintf(stderr, "失敗：FIFO 鎖互斥，計數 %ld 應為 %ld\n", counter, expect);
      failures++;
    } else {
      printf("通過：FIFO 鎖互斥（%d 執行緒 × %d 次）\n", FLT_THREADS, FLT_ROUNDS);
    }
  }

  // ---- 遞迴 ----
  {
    ntfsb_fairlock_lock(&lock);
    ntfsb_fairlock_lock(&lock);
    ntfsb_fairlock_lock(&lock);
    ntfsb_fairlock_unlock(&lock);
    ntfsb_fairlock_unlock(&lock);
    ntfsb_fairlock_unlock(&lock);
    // 全部放開後別的執行緒才能取得；用貪婪執行緒確認沒有卡住
    flt_greedy_ctx g = { .lock = &lock, .stop = 0, .cycles = 0 };
    pthread_t th;
    pthread_create(&th, NULL, flt_greedy_thread, &g);
    double deadline = flt_now() + 1.0;
    while (g.cycles == 0 && flt_now() < deadline) {
      // 等貪婪執行緒至少取得一次
    }
    g.stop = 1;
    pthread_join(th, NULL);
    if (g.cycles == 0) {
      fprintf(stderr, "失敗：FIFO 鎖遞迴後無法被其他執行緒取得\n");
      failures++;
    } else {
      printf("通過：FIFO 鎖遞迴取得與釋放\n");
    }
  }

  // ---- 不飢餓 ----
  // 這是換掉 NSRecursiveLock 的理由：一條持續取鎖的執行緒不該讓別人等到天荒地老。
  {
    flt_greedy_ctx g = { .lock = &lock, .stop = 0, .cycles = 0 };
    pthread_t th;
    pthread_create(&th, NULL, flt_greedy_thread, &g);

    const int samples = 200;
    double worst = 0;
    for (int i = 0; i < samples; i++) {
      double t0 = flt_now();
      ntfsb_fairlock_lock(&lock);
      double waited = flt_now() - t0;
      ntfsb_fairlock_unlock(&lock);
      if (waited > worst) {
        worst = waited;
      }
    }
    g.stop = 1;
    pthread_join(th, NULL);

    // 公平鎖下每次最多等一個臨界區（次微秒等級）。門檻放到 100 ms 仍能抓出
    // 飢餓——量測顯示不公平的鎖會到 1.5 秒。
    if (worst > 0.100) {
      fprintf(stderr, "失敗：FIFO 鎖出現飢餓，最長等待 %.1f ms（貪婪執行緒取得 %ld 次）\n",
              worst * 1000.0, g.cycles);
      failures++;
    } else {
      printf("通過：FIFO 鎖不飢餓（最長等待 %.3f ms，貪婪執行緒取得 %ld 次）\n",
             worst * 1000.0, g.cycles);
    }
  }

  ntfsb_fairlock_destroy(&lock);
  return failures;
}
