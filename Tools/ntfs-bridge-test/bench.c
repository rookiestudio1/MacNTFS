//
//  bench.c
//  ntfs-bridge-test
//

#include "bench.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "file_device.h"
#include "ntfs_bridge.h"

// ---- 量測參數 ----
#define BENCH_SEQ_BYTES      (128ull * 1024 * 1024)  ///< 循序讀寫總量
#define BENCH_SEQ_CHUNK      (128ull * 1024)         ///< 循序讀寫每次大小
#define BENCH_RAND_COUNT     4096                    ///< 隨機讀取次數
#define BENCH_RAND_CHUNK     4096                    ///< 隨機讀取每次大小
#define BENCH_SMALL_COUNT    2000                    ///< 小檔數量
#define BENCH_SMALL_SIZE     1024                    ///< 小檔大小

/// 單一項目的量測結果
typedef struct bench_result {
  const char *name;
  double seconds;
  uint64_t logical_bytes;  ///< 邏輯上處理的位元組數（0 表示這項以次數計）
  uint64_t ops;            ///< 邏輯操作次數
  const char *op_unit;     ///< ops 的單位名稱，例如 "檔/秒"
  file_device_stats io;    ///< 期間的裝置 I/O 統計
} bench_result;

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/// 可重現的簡易亂數，讓每次執行的隨機讀取位置一致，前後對照才有意義
static uint64_t bench_rand(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static void print_header(void) {
  printf("\n%-14s %10s %12s %10s %10s %12s %10s %8s\n",
         "項目", "耗時(s)", "吞吐", "讀次數", "寫次數",
         "裝置讀寫(MB)", "放大倍率", "sync");
  printf("%s\n", "--------------------------------------------------"
                 "--------------------------------------------------");
}

static void print_result(const bench_result *r) {
  char throughput[32];
  if (r->logical_bytes > 0) {
    double mbps = ((double)r->logical_bytes / (1024.0 * 1024.0)) / r->seconds;
    snprintf(throughput, sizeof(throughput), "%.1f MB/s", mbps);
  } else {
    snprintf(throughput, sizeof(throughput), "%.0f %s",
             (double)r->ops / r->seconds, r->op_unit ? r->op_unit : "op/s");
  }

  uint64_t dev_bytes = r->io.read_bytes + r->io.write_bytes;
  double dev_mb = (double)dev_bytes / (1024.0 * 1024.0);

  // 放大倍率：裝置實際搬運的位元組數 / 邏輯上要求的位元組數。
  // 這是橋接層效率最直接的指標——runlist 重複重建、MFT 反覆寫回都會讓它變大。
  char amp[16];
  if (r->logical_bytes > 0) {
    snprintf(amp, sizeof(amp), "%.2fx", (double)dev_bytes / (double)r->logical_bytes);
  } else {
    snprintf(amp, sizeof(amp), "-");
  }

  printf("%-14s %10.3f %12s %10" PRIu64 " %10" PRIu64 " %12.1f %10s %8" PRIu64 "\n",
         r->name, r->seconds, throughput,
         r->io.read_calls, r->io.write_calls, dev_mb, amp, r->io.sync_calls);
}

/// 取得目前統計與起始統計的差值
static file_device_stats stats_delta(const file_device_stats *before,
                                     const file_device_stats *after) {
  file_device_stats d = {
    .read_calls  = after->read_calls  - before->read_calls,
    .read_bytes  = after->read_bytes  - before->read_bytes,
    .write_calls = after->write_calls - before->write_calls,
    .write_bytes = after->write_bytes - before->write_bytes,
    .sync_calls  = after->sync_calls  - before->sync_calls,
  };
  return d;
}


// MARK: - 混合負載下的中繼資料延遲
//
// 橋接層本身不做序列化（呼叫端負責），所以這裡用一把互斥鎖複製 NTFSVolume
// 目前的粗鎖，量到的就是今天實際的行為：大檔傳輸進行中，一個 getattr 要等多久。
// 這是「複製檔案時 Finder 卡住」的直接量化。

typedef struct bench_load_ctx {
  ntfsb_volume *vol;
  pthread_mutex_t *lock;
  uint64_t mft;
  volatile int stop;
  uint64_t chunks;
} bench_load_ctx;

static void *bench_load_thread(void *arg) {
  bench_load_ctx *c = arg;
  uint8_t *buf = malloc(BENCH_SEQ_CHUNK);
  if (!buf) {
    return NULL;
  }
  int64_t off = 0;
  while (!c->stop) {
    pthread_mutex_lock(c->lock);
    int64_t r = ntfsb_read(c->vol, c->mft, off, buf, BENCH_SEQ_CHUNK);
    pthread_mutex_unlock(c->lock);
    if (r <= 0) {
      off = 0;
    } else {
      off += r;
      c->chunks++;
      if ((uint64_t)off >= BENCH_SEQ_BYTES) {
        off = 0;
      }
    }
  }
  free(buf);
  return NULL;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/// 量 count 次 getattr 的延遲分布（微秒）
static void bench_metadata_latency(ntfsb_volume *vol,
                                   pthread_mutex_t *lock,
                                   const uint64_t *mfts,
                                   int count,
                                   double *p50,
                                   double *p99,
                                   double *pmax) {
  double *samples = malloc(sizeof(double) * (size_t)count);
  if (!samples) {
    *p50 = *p99 = *pmax = 0;
    return;
  }
  for (int i = 0; i < count; i++) {
    ntfsb_attr a;
    double t = now_seconds();
    pthread_mutex_lock(lock);
    ntfsb_getattr(vol, mfts[i], &a);
    pthread_mutex_unlock(lock);
    samples[i] = (now_seconds() - t) * 1e6;
  }
  qsort(samples, (size_t)count, sizeof(double), cmp_double);
  *p50 = samples[count / 2];
  *p99 = samples[(int)((double)count * 0.99)];
  *pmax = samples[count - 1];
  free(samples);
}

/// 目錄走訪用：把每個項目的 MFT 編號收集起來，之後逐一 getattr
typedef struct bench_walk_ctx {
  uint64_t *mfts;
  int count;
  int cap;
} bench_walk_ctx;

static int bench_walk_cb(void *context,
                         const char *name,
                         size_t name_len,
                         uint64_t mft_no,
                         ntfsb_type type,
                         int64_t cookie,
                         int64_t next_cookie) {
  (void)name;
  (void)name_len;
  (void)type;
  (void)cookie;
  (void)next_cookie;
  bench_walk_ctx *c = context;
  if (c->count >= c->cap) {
    return 1;  // 緩衝區已滿，停止列舉
  }
  c->mfts[c->count++] = mft_no;
  return 0;
}

int bench_run(const char *image_path, uint32_t block_size) {
  file_device dev;
  if (file_device_open(&dev, image_path) < 0) {
    perror("開啟映像檔");
    return 2;
  }
  ntfsb_device_callbacks cb = file_device_callbacks(&dev, block_size);

  ntfsb_global_init();

  int err = 0;
  ntfsb_volume *vol = ntfsb_mount("bench", &cb, false, &err);
  if (!vol) {
    fprintf(stderr, "掛載失敗：errno=%d %s\n", err, strerror(err));
    file_device_close(&dev);
    return 1;
  }
  if (ntfsb_is_readonly(vol)) {
    fprintf(stderr, "卷被掛載為唯讀，無法量測寫入\n");
    ntfsb_umount(vol);
    file_device_close(&dev);
    return 1;
  }

  ntfsb_statfs_info sf;
  if (ntfsb_statfs(vol, &sf) == 0) {
    printf("映像檔 %s：叢集大小 %u，可用 %.1f MB\n",
           image_path, sf.cluster_size,
           (double)sf.free_clusters * sf.cluster_size / (1024.0 * 1024.0));
  }
  printf("循序 %llu MB（每次 %llu KB）／隨機 %d × %d B／小檔 %d × %d B\n",
         BENCH_SEQ_BYTES / (1024 * 1024), BENCH_SEQ_CHUNK / 1024,
         BENCH_RAND_COUNT, BENCH_RAND_CHUNK, BENCH_SMALL_COUNT, BENCH_SMALL_SIZE);

  uint8_t *buf = malloc(BENCH_SEQ_CHUNK);
  if (!buf) {
    fprintf(stderr, "配置緩衝區失敗\n");
    ntfsb_umount(vol);
    file_device_close(&dev);
    return 1;
  }
  memset(buf, 0xA5, BENCH_SEQ_CHUNK);

  bench_result results[6];
  int n = 0;
  int rc = 0;
  file_device_stats before, after;
  double t0;

  // ---- 1. 循序寫入 ----
  uint64_t big_mft = 0;
  rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "bench-seq.dat", NTFSB_TYPE_FILE, NULL, &big_mft);
  if (rc) {
    fprintf(stderr, "建立 bench-seq.dat 失敗：rc=%d\n", rc);
    goto fail;
  }
  before = dev.stats;
  t0 = now_seconds();
  {
    int ferr = 0;
    ntfsb_file *f = ntfsb_file_open(vol, big_mft, &ferr);
    if (!f) {
      fprintf(stderr, "開啟 handle 失敗：errno=%d\n", ferr);
      rc = 1;
      goto fail;
    }
    for (uint64_t off = 0; off < BENCH_SEQ_BYTES; off += BENCH_SEQ_CHUNK) {
      int64_t w = ntfsb_file_write(f, (int64_t)off, buf, BENCH_SEQ_CHUNK);
      if (w != (int64_t)BENCH_SEQ_CHUNK) {
        fprintf(stderr, "循序寫入失敗 offset=%llu rc=%lld\n", off, (long long)w);
        ntfsb_file_close(f);
        rc = 1;
        goto fail;
      }
    }
    ntfsb_file_close(f);
  }
  ntfsb_sync(vol);
  results[n].seconds = now_seconds() - t0;
  after = dev.stats;
  results[n].name = "循序寫入";
  results[n].logical_bytes = BENCH_SEQ_BYTES;
  results[n].ops = BENCH_SEQ_BYTES / BENCH_SEQ_CHUNK;
  results[n].op_unit = NULL;
  results[n].io = stats_delta(&before, &after);
  n++;

  // ---- 2. 循序讀取 ----
  before = dev.stats;
  t0 = now_seconds();
  {
    int ferr = 0;
    ntfsb_file *f = ntfsb_file_open(vol, big_mft, &ferr);
    if (!f) {
      fprintf(stderr, "開啟 handle 失敗：errno=%d\n", ferr);
      rc = 1;
      goto fail;
    }
    for (uint64_t off = 0; off < BENCH_SEQ_BYTES; off += BENCH_SEQ_CHUNK) {
      int64_t r = ntfsb_file_read(f, (int64_t)off, buf, BENCH_SEQ_CHUNK);
      if (r != (int64_t)BENCH_SEQ_CHUNK) {
        fprintf(stderr, "循序讀取失敗 offset=%llu rc=%lld\n", off, (long long)r);
        ntfsb_file_close(f);
        rc = 1;
        goto fail;
      }
    }
    ntfsb_file_close(f);
  }
  results[n].seconds = now_seconds() - t0;
  after = dev.stats;
  results[n].name = "循序讀取";
  results[n].logical_bytes = BENCH_SEQ_BYTES;
  results[n].ops = BENCH_SEQ_BYTES / BENCH_SEQ_CHUNK;
  results[n].op_unit = NULL;
  results[n].io = stats_delta(&before, &after);
  n++;

  // ---- 3. 循序覆寫（檔案已配置，大小不變）----
  // 與「循序寫入」對照可以把「配置新叢集的成本」和「每次寫入的中繼資料成本」分開。
  before = dev.stats;
  t0 = now_seconds();
  {
    int ferr = 0;
    ntfsb_file *f = ntfsb_file_open(vol, big_mft, &ferr);
    if (!f) {
      fprintf(stderr, "開啟 handle 失敗：errno=%d\n", ferr);
      rc = 1;
      goto fail;
    }
    for (uint64_t off = 0; off < BENCH_SEQ_BYTES; off += BENCH_SEQ_CHUNK) {
      int64_t w = ntfsb_file_write(f, (int64_t)off, buf, BENCH_SEQ_CHUNK);
      if (w != (int64_t)BENCH_SEQ_CHUNK) {
        fprintf(stderr, "循序覆寫失敗 offset=%llu rc=%lld\n", off, (long long)w);
        ntfsb_file_close(f);
        rc = 1;
        goto fail;
      }
    }
    ntfsb_file_close(f);
  }
  ntfsb_sync(vol);
  results[n].seconds = now_seconds() - t0;
  after = dev.stats;
  results[n].name = "循序覆寫";
  results[n].logical_bytes = BENCH_SEQ_BYTES;
  results[n].ops = BENCH_SEQ_BYTES / BENCH_SEQ_CHUNK;
  results[n].op_unit = NULL;
  results[n].io = stats_delta(&before, &after);
  n++;

  // ---- 4. 隨機讀取 ----
  {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t max_off = BENCH_SEQ_BYTES - BENCH_RAND_CHUNK;
    before = dev.stats;
    t0 = now_seconds();
    {
      int ferr = 0;
      ntfsb_file *f = ntfsb_file_open(vol, big_mft, &ferr);
      if (!f) {
        fprintf(stderr, "開啟 handle 失敗：errno=%d\n", ferr);
        rc = 1;
        goto fail;
      }
      for (int i = 0; i < BENCH_RAND_COUNT; i++) {
        uint64_t off = bench_rand(&state) % max_off;
        off -= off % BENCH_RAND_CHUNK;
        int64_t r = ntfsb_file_read(f, (int64_t)off, buf, BENCH_RAND_CHUNK);
        if (r != BENCH_RAND_CHUNK) {
          fprintf(stderr, "隨機讀取失敗 offset=%llu rc=%lld\n", off, (long long)r);
          ntfsb_file_close(f);
          rc = 1;
          goto fail;
        }
      }
      ntfsb_file_close(f);
    }
    results[n].seconds = now_seconds() - t0;
    after = dev.stats;
    results[n].name = "隨機讀取";
    results[n].logical_bytes = (uint64_t)BENCH_RAND_COUNT * BENCH_RAND_CHUNK;
    results[n].ops = BENCH_RAND_COUNT;
    results[n].op_unit = NULL;
    results[n].io = stats_delta(&before, &after);
    n++;
  }

  // ---- 5. 小檔建立 ----
  uint64_t small_dir = 0;
  rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "bench-small", NTFSB_TYPE_DIR, NULL, &small_dir);
  if (rc) {
    fprintf(stderr, "建立 bench-small 目錄失敗：rc=%d\n", rc);
    goto fail;
  }
  before = dev.stats;
  // 設定 BENCH_TRACE=<路徑> 可把這個階段的每次裝置 I/O 記下來，用於歸因流量
  {
    const char *trace_path = getenv("BENCH_TRACE");
    if (trace_path && file_device_trace(&dev, trace_path) != 0) {
      fprintf(stderr, "無法開啟軌跡檔 %s\n", trace_path);
    }
  }
  t0 = now_seconds();
  for (int i = 0; i < BENCH_SMALL_COUNT; i++) {
    char name[64];
    snprintf(name, sizeof(name), "f%05d.bin", i);
    uint64_t mft = 0;
    rc = ntfsb_create(vol, small_dir, name, NTFSB_TYPE_FILE, NULL, &mft);
    if (rc) {
      fprintf(stderr, "建立 %s 失敗：rc=%d\n", name, rc);
      goto fail;
    }
    int ferr = 0;
    ntfsb_file *f = ntfsb_file_open(vol, mft, &ferr);
    if (!f) {
      fprintf(stderr, "開啟 %s 的 handle 失敗：errno=%d\n", name, ferr);
      rc = 1;
      goto fail;
    }
    int64_t w = ntfsb_file_write(f, 0, buf, BENCH_SMALL_SIZE);
    ntfsb_file_close(f);
    if (w != BENCH_SMALL_SIZE) {
      fprintf(stderr, "寫入 %s 失敗：rc=%lld\n", name, (long long)w);
      rc = 1;
      goto fail;
    }
  }
  ntfsb_sync(vol);
  results[n].seconds = now_seconds() - t0;
  after = dev.stats;
  file_device_trace(&dev, NULL);
  results[n].name = "小檔建立";
  results[n].logical_bytes = 0;
  results[n].ops = BENCH_SMALL_COUNT;
  results[n].op_unit = "檔/秒";
  results[n].io = stats_delta(&before, &after);
  n++;

  // ---- 6. 目錄走訪（readdir + 每項 getattr）----
  {
    // readdir 回呼把 mft 編號收集起來，之後逐一 getattr
    bench_walk_ctx wc = { .mfts = malloc(sizeof(uint64_t) * (BENCH_SMALL_COUNT + 8)),
                          .count = 0,
                          .cap = BENCH_SMALL_COUNT + 8 };
    if (!wc.mfts) {
      fprintf(stderr, "配置走訪緩衝區失敗\n");
      rc = 1;
      goto fail;
    }

    before = dev.stats;
    t0 = now_seconds();
    int64_t pos = NTFSB_COOKIE_START;
    while (pos != NTFSB_COOKIE_END) {
      rc = ntfsb_readdir(vol, small_dir, &pos, bench_walk_cb, &wc);
      if (rc) {
        fprintf(stderr, "readdir 失敗：rc=%d\n", rc);
        free(wc.mfts);
        goto fail;
      }
    }
    for (int i = 0; i < wc.count; i++) {
      ntfsb_attr a;
      rc = ntfsb_getattr(vol, wc.mfts[i], &a);
      if (rc) {
        fprintf(stderr, "getattr 失敗：mft=%llu rc=%d\n",
                (unsigned long long)wc.mfts[i], rc);
        free(wc.mfts);
        goto fail;
      }
    }
    results[n].seconds = now_seconds() - t0;
    after = dev.stats;
    results[n].name = "目錄走訪";
    results[n].logical_bytes = 0;
    results[n].ops = (uint64_t)wc.count;
    results[n].op_unit = "項/秒";
    results[n].io = stats_delta(&before, &after);
    n++;

    // ---- 7. 混合負載下的中繼資料延遲 ----
    {
      int sample_count = wc.count < 2000 ? wc.count : 2000;
      pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
      double idle50 = 0, idle99 = 0, idlemax = 0;
      bench_metadata_latency(vol, &lock, wc.mfts, sample_count, &idle50, &idle99, &idlemax);

      bench_load_ctx ctx = { .vol = vol, .lock = &lock, .mft = big_mft, .stop = 0, .chunks = 0 };
      pthread_t th;
      double busy50 = 0, busy99 = 0, busymax = 0;
      if (pthread_create(&th, NULL, bench_load_thread, &ctx) == 0) {
        bench_metadata_latency(vol, &lock, wc.mfts, sample_count, &busy50, &busy99, &busymax);
        ctx.stop = 1;
        pthread_join(th, NULL);
      }

      printf("\n中繼資料延遲（getattr × %d，微秒）\n", sample_count);
      printf("  %-22s p50 %8.1f  p99 %8.1f  最大 %8.1f\n", "無背景負載", idle50, idle99, idlemax);
      printf("  %-22s p50 %8.1f  p99 %8.1f  最大 %8.1f  （背景讀取 %llu 個 128 KB 區塊）\n",
             "大檔讀取進行中", busy50, busy99, busymax, (unsigned long long)ctx.chunks);
    }
    free(wc.mfts);
  }

  print_header();
  for (int i = 0; i < n; i++) {
    print_result(&results[i]);
  }
  printf("\n");
  rc = 0;

fail:
  free(buf);
  ntfsb_umount(vol);
  file_device_close(&dev);
  return rc;
}
