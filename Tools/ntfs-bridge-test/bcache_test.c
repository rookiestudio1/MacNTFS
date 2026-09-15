//
//  bcache_test.c
//  ntfs-bridge-test
//
//  區塊快取的對照測試：用記憶體裝置跑隨機讀寫，每一步都和參考緩衝區比對。
//  快取是 write-back，所以只有在 flush 之後才要求裝置內容與參考一致。
//

#include "bcache_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntfs_blockcache.h"

#define BCT_BLOCK   512
#define BCT_BLOCKS  4096                       // 2 MB，刻意小於快取容量的一半以外
#define BCT_SIZE    (BCT_BLOCK * BCT_BLOCKS)

typedef struct mem_device {
  uint8_t *disk;
  int64_t size;
  uint64_t writes;  ///< pwrite 呼叫次數
} mem_device;

static int64_t mem_pread(void *ctx, void *buf, int64_t count, int64_t offset) {
  mem_device *d = ctx;
  if (offset < 0 || offset + count > d->size) {
    return -1;
  }
  memcpy(buf, d->disk + offset, (size_t)count);
  return count;
}

static int64_t mem_pwrite(void *ctx, const void *buf, int64_t count, int64_t offset) {
  mem_device *d = ctx;
  if (offset < 0 || offset + count > d->size) {
    return -1;
  }
  memcpy(d->disk + offset, buf, (size_t)count);
  d->writes++;
  return count;
}

static int mem_sync(void *ctx) {
  (void)ctx;
  return 0;
}

static uint64_t bct_rand(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

#define BCT_CHECK(cond, msg)                                    \
  do {                                                          \
    if (!(cond)) {                                              \
      failures++;                                               \
      fprintf(stderr, "失敗 [%s:%d] %s\n", __FILE__, __LINE__, msg); \
    } else {                                                    \
      printf("通過：%s\n", msg);                                 \
    }                                                           \
  } while (0)

/// flush_range：範圍內髒行落盤且失效，範圍外髒行保持不動
static int bcache_test_flush_range(void) {
  int failures = 0;
  mem_device dev = { .disk = calloc(1, BCT_SIZE), .size = BCT_SIZE };
  ntfsb_device_callbacks cb = {
    .context = &dev,
    .block_size = BCT_BLOCK,
    .block_count = BCT_BLOCKS,
    .writable = true,
    .pread = mem_pread,
    .pwrite = mem_pwrite,
    .sync = mem_sync,
  };
  ntfsb_bcache *c = ntfsb_bcache_create(&cb, BCT_SIZE);
  const int64_t L = NTFSB_BCACHE_LINE;
  uint8_t a[NTFSB_BCACHE_LINE], b[NTFSB_BCACHE_LINE], out[NTFSB_BCACHE_LINE];
  memset(a, 0xA1, sizeof(a));
  memset(b, 0xB2, sizeof(b));
  // 行 3 與行 10 各寫一行（都留在快取裡）
  BCT_CHECK(ntfsb_bcache_pwrite(c, a, L, 3 * L) == L, "flush_range：寫入行 3");
  BCT_CHECK(ntfsb_bcache_pwrite(c, b, L, 10 * L) == L, "flush_range：寫入行 10");
  uint64_t writes_before = dev.writes;
  BCT_CHECK(ntfsb_bcache_flush_range(c, 3 * L, L) == 0, "flush_range：行 3");
  BCT_CHECK(dev.writes == writes_before + 1, "flush_range：只寫回一行");
  BCT_CHECK(memcmp(dev.disk + 3 * L, a, L) == 0, "flush_range：行 3 已在裝置上");
  BCT_CHECK(memcmp(dev.disk + 10 * L, b, L) != 0, "flush_range：行 10 仍未落盤");
  // 失效：直接改裝置內容後再經快取讀，必須看到新內容
  memset(dev.disk + 3 * L, 0xC3, L);
  BCT_CHECK(ntfsb_bcache_pread(c, out, L, 3 * L) == L && out[0] == 0xC3,
            "flush_range：行 3 已失效，重讀拿到裝置新內容");
  // 超大範圍（超過快取行數）也要正確，且走逐槽掃描而不是逐行查雜湊
  BCT_CHECK(ntfsb_bcache_flush_range(c, 0, BCT_SIZE) == 0, "flush_range：整個裝置");
  BCT_CHECK(memcmp(dev.disk + 10 * L, b, L) == 0, "flush_range：行 10 這次落盤了");
  ntfsb_bcache_free(c);
  free(dev.disk);
  return failures;
}

int bcache_test_run(void) {
  int failures = bcache_test_flush_range();
  mem_device dev = { .disk = calloc(1, BCT_SIZE), .size = BCT_SIZE };
  uint8_t *ref = calloc(1, BCT_SIZE);
  uint8_t *buf = malloc(BCT_SIZE);
  if (!dev.disk || !ref || !buf) {
    fprintf(stderr, "區塊快取測試：配置記憶體失敗\n");
    free(dev.disk); free(ref); free(buf);
    return 1;
  }
  // 讓初始內容不是全零，才能抓出「該讀卻沒讀」的錯誤
  for (int i = 0; i < BCT_SIZE; i++) {
    dev.disk[i] = (uint8_t)(i * 7 + 3);
  }
  memcpy(ref, dev.disk, BCT_SIZE);

  ntfsb_device_callbacks cb = {
    .context = &dev,
    .block_size = BCT_BLOCK,
    .block_count = BCT_BLOCKS,
    .writable = true,
    .pread = mem_pread,
    .pwrite = mem_pwrite,
    .sync = mem_sync,
  };
  ntfsb_bcache *c = ntfsb_bcache_create(&cb, BCT_SIZE);
  if (!c) {
    fprintf(stderr, "區塊快取測試：建立失敗\n");
    free(dev.disk); free(ref); free(buf);
    return 1;
  }

  uint64_t seed = 0xC0FFEEull;
  const int rounds = 20000;
  for (int i = 0; i < rounds && failures == 0; i++) {
    // 涵蓋小請求與超過 bypass 門檻的大請求
    int64_t blocks = (int64_t)(bct_rand(&seed) % 200) + 1;
    int64_t count = blocks * BCT_BLOCK;
    if (count > BCT_SIZE) {
      count = BCT_SIZE;
    }
    int64_t maxblk = (BCT_SIZE - count) / BCT_BLOCK;
    int64_t offset = (int64_t)(bct_rand(&seed) % (uint64_t)(maxblk + 1)) * BCT_BLOCK;

    if (bct_rand(&seed) & 1) {
      // 寫入
      uint8_t pattern = (uint8_t)(bct_rand(&seed) & 0xFF);
      memset(buf, pattern, (size_t)count);
      int64_t w = ntfsb_bcache_pwrite(c, buf, count, offset);
      if (w != count) {
        fprintf(stderr, "區塊快取測試：第 %d 輪寫入回傳 %lld（應為 %lld）\n",
                i, (long long)w, (long long)count);
        failures++;
        break;
      }
      memset(ref + offset, pattern, (size_t)count);
    } else {
      // 讀取並與參考比對
      int64_t r = ntfsb_bcache_pread(c, buf, count, offset);
      if (r != count) {
        fprintf(stderr, "區塊快取測試：第 %d 輪讀取回傳 %lld（應為 %lld）\n",
                i, (long long)r, (long long)count);
        failures++;
        break;
      }
      if (memcmp(buf, ref + offset, (size_t)count) != 0) {
        int64_t k = 0;
        while (k < count && buf[k] == ref[offset + k]) {
          k++;
        }
        fprintf(stderr,
                "區塊快取測試：第 %d 輪讀取內容不符 offset=%lld count=%lld "
                "首個差異在 +%lld（得到 0x%02x 應為 0x%02x）\n",
                i, (long long)offset, (long long)count, (long long)k,
                buf[k], ref[offset + k]);
        failures++;
        break;
      }
    }

    // 偶爾 flush，flush 後裝置內容必須與參考完全一致
    if ((i % 97) == 0) {
      if (ntfsb_bcache_flush(c) != 0) {
        fprintf(stderr, "區塊快取測試：第 %d 輪 flush 失敗\n", i);
        failures++;
        break;
      }
      if (memcmp(dev.disk, ref, BCT_SIZE) != 0) {
        int64_t k = 0;
        while (k < BCT_SIZE && dev.disk[k] == ref[k]) {
          k++;
        }
        fprintf(stderr,
                "區塊快取測試：第 %d 輪 flush 後裝置內容不符，"
                "首個差異在位移 %lld（行 %lld，得到 0x%02x 應為 0x%02x）\n",
                i, (long long)k, (long long)(k / NTFSB_BCACHE_LINE),
                dev.disk[k], ref[k]);
        failures++;
        break;
      }
    }
  }

  if (failures == 0) {
    if (ntfsb_bcache_flush(c) != 0 || memcmp(dev.disk, ref, BCT_SIZE) != 0) {
      fprintf(stderr, "區塊快取測試：最終 flush 後內容不符\n");
      failures++;
    }
  }

  ntfsb_bcache_free(c);
  free(dev.disk);
  free(ref);
  free(buf);
  printf("%s：區塊快取隨機讀寫對照（%d 輪）\n", failures ? "失敗" : "通過", rounds);
  return failures;
}
