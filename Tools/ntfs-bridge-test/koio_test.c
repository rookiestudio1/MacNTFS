//
//  koio_test.c
//  ntfs-bridge-test
//
//  Kernel offloaded I/O（KOIO）的橋接層測試。
//  真正的 kernel 會拿 blockmap 給的 extent 直接對裝置 I/O，這裡用 file_device
//  的 fd 做同樣的事，再用橋接層讀回比對，驗證 extent 正確、中繼資料更新正確、
//  以及 write-back 區塊快取不會讓兩邊看到不同的資料。
//

#include "koio_test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      failures++;                                               \
      fprintf(stderr, "失敗 [%s:%d] ", __FILE__, __LINE__);     \
      fprintf(stderr, __VA_ARGS__);                             \
      fprintf(stderr, "\n");                                    \
    } else {                                                    \
      printf("通過：");                                          \
      printf(__VA_ARGS__);                                      \
      printf("\n");                                             \
    }                                                           \
  } while (0)

#define CLUSTER 4096

/// 合格規則只看 ntfsb_attr，不需要真的檔案
static void test_eligibility(void) {
  ntfsb_attr a;
  memset(&a, 0, sizeof(a));
  a.type = NTFSB_TYPE_FILE;
  a.size = 1 << 20;
  a.init_size = a.size;
  CHECK(ntfsb_koio_eligible(&a, CLUSTER), "合格：一般非常駐檔案");

  a.resident = true;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：常駐");
  a.resident = false;

  a.compressed = true;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：壓縮");
  a.compressed = false;

  a.encrypted = true;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：加密");
  a.encrypted = false;

  a.sparse = true;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：sparse");
  a.sparse = false;

  a.init_size = 8 * CLUSTER;
  CHECK(ntfsb_koio_eligible(&a, CLUSTER), "合格：init_size 小於 size 但對齊叢集");
  a.init_size = 8 * CLUSTER + 100;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：init_size 未對齊叢集");
  a.init_size = a.size;

  a.type = NTFSB_TYPE_DIR;
  CHECK(!ntfsb_koio_eligible(&a, CLUSTER), "不合格：目錄");
  a.type = NTFSB_TYPE_FILE;

  a.size = 0;
  a.init_size = 0;
  CHECK(ntfsb_koio_eligible(&a, CLUSTER), "合格：非常駐空檔");
}

/// 建立一個非常駐、內容為 fill 的檔案，回傳 mft_no（失敗回 0）
static uint64_t make_file(ntfsb_volume *vol, const char *name, size_t size, uint8_t fill) {
  uint64_t mft = 0;
  if (ntfsb_create(vol, NTFSB_ROOT_MFT_NO, name, NTFSB_TYPE_FILE, NULL, &mft) != 0) {
    return 0;
  }
  if (size == 0) {
    return mft;
  }
  uint8_t *buf = malloc(size);
  memset(buf, fill, size);
  int64_t w = ntfsb_write(vol, mft, 0, buf, size);
  free(buf);
  return (w == (int64_t)size) ? mft : 0;
}

/// 拉長不留洞：ftruncate 後循序寫的檔案必須仍可走 KOIO
static void test_truncate_solid(ntfsb_volume *vol) {
  uint64_t mft = make_file(vol, "koio_trunc.bin", 8192, 0x11);
  CHECK(mft != 0, "truncate_solid：建立 8 KB 檔案");
  if (!mft) {
    return;
  }
  int rc = ntfsb_truncate(vol, mft, 4 << 20);
  CHECK(rc == 0, "truncate_solid：拉長到 4 MB (rc=%d)", rc);
  ntfsb_attr a;
  rc = ntfsb_getattr(vol, mft, &a);
  CHECK(rc == 0 && a.size == (4u << 20), "truncate_solid：大小 4 MB");
  CHECK(!a.sparse, "truncate_solid：沒有變成 sparse");
  CHECK(a.alloc_size >= (4u << 20), "truncate_solid：叢集真的配置了 (alloc=%llu)",
        (unsigned long long)a.alloc_size);
  CHECK(a.init_size == 8192, "truncate_solid：init_size 停在 8 KB (%llu)",
        (unsigned long long)a.init_size);
  CHECK(ntfsb_koio_eligible(&a, CLUSTER), "truncate_solid：仍合格");
  // 超過 init 的部分讀回必須是零
  uint8_t buf[4096];
  int64_t r = ntfsb_read(vol, mft, 1 << 20, buf, sizeof(buf));
  CHECK(r == (int64_t)sizeof(buf) && buf[0] == 0 && buf[4095] == 0, "truncate_solid：未初始化區讀到零");
}

/// 假 kernel：收集 extent
typedef struct extent_list {
  ntfsb_extent items[64];
  int count;
  int limit;   ///< 模擬 packer 容量：達到後回 1
} extent_list;

static int collect_extent(void *ctx, const ntfsb_extent *e) {
  extent_list *l = ctx;
  if (l->limit && l->count >= l->limit) {
    return 1;
  }
  if (l->count < 64) {
    l->items[l->count++] = *e;
  }
  return 0;
}

/// 假 kernel：依 extent 直接讀映像檔，組出 [offset, offset+len) 的內容
static int kernel_read(file_device *dev, const extent_list *l, int64_t offset, int64_t len, uint8_t *out) {
  memset(out, 0xEE, len);  // 沒被 extent 覆蓋的位置留下記號
  for (int i = 0; i < l->count; i++) {
    const ntfsb_extent *e = &l->items[i];
    int64_t lo = e->logical > offset ? e->logical : offset;
    int64_t hi = (e->logical + e->length < offset + len) ? e->logical + e->length : offset + len;
    if (hi <= lo) {
      continue;
    }
    if (e->zero_fill) {
      memset(out + (lo - offset), 0, hi - lo);
    } else if (pread(dev->fd, out + (lo - offset), hi - lo, e->physical + (lo - e->logical)) != hi - lo) {
      return -1;
    }
  }
  return 0;
}

/// 檢查 extent 依序、不重疊、完整覆蓋 [offset, offset+len)
static bool extents_cover(const extent_list *l, int64_t offset, int64_t len) {
  int64_t pos = offset;
  for (int i = 0; i < l->count; i++) {
    const ntfsb_extent *e = &l->items[i];
    if (e->logical > pos) {
      return false;
    }
    if (e->logical + e->length > pos) {
      pos = e->logical + e->length;
    }
  }
  return pos >= offset + len;
}

static void test_blockmap_read(ntfsb_volume *vol, file_device *dev) {
  // 3 MB 圖樣檔，再拉長到 5 MB 讓後段是未初始化區
  const size_t data_len = 3 << 20;
  uint8_t *pattern = malloc(data_len);
  for (size_t i = 0; i < data_len; i++) {
    pattern[i] = (uint8_t)(i * 7 + (i >> 12));
  }
  uint64_t mft = 0;
  int rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "koio_read.bin", NTFSB_TYPE_FILE, NULL, &mft);
  CHECK(rc == 0, "blockmap 讀：建立檔案 (rc=%d)", rc);
  int64_t w = ntfsb_write(vol, mft, 0, pattern, data_len);
  CHECK(w == (int64_t)data_len, "blockmap 讀：寫入 3 MB");
  rc = ntfsb_truncate(vol, mft, 5 << 20);
  CHECK(rc == 0, "blockmap 讀：拉長到 5 MB");
  CHECK(ntfsb_sync(vol) == 0, "blockmap 讀：sync");

  // 未對齊的範圍：從 100 到 4 MB + 50
  int64_t off = 100, len = (4 << 20) - 50;
  extent_list l = {0};
  rc = ntfsb_blockmap(vol, mft, off, len, false, collect_extent, &l);
  CHECK(rc == 0 && l.count > 0, "blockmap 讀：rc=%d extents=%d", rc, l.count);
  CHECK(extents_cover(&l, off, len), "blockmap 讀：extent 完整覆蓋且不重疊");
  bool aligned = true;
  for (int i = 0; i < l.count; i++) {
    if (!l.items[i].zero_fill && (l.items[i].physical % CLUSTER || l.items[i].logical % CLUSTER)) {
      aligned = false;
    }
  }
  CHECK(aligned, "blockmap 讀：Data extent 都對齊叢集");
  CHECK(l.count > 0 && l.items[l.count - 1].zero_fill, "blockmap 讀：超過 init_size 的部分是 ZeroFill");

  uint8_t *got = malloc(len);
  uint8_t *want = malloc(len);
  CHECK(kernel_read(dev, &l, off, len, got) == 0, "blockmap 讀：假 kernel 讀取");
  int64_t r = ntfsb_read(vol, mft, off, want, len);
  CHECK(r == len, "blockmap 讀：橋接層讀取 (r=%lld)", (long long)r);
  CHECK(memcmp(got, want, len) == 0, "blockmap 讀：kernel 與橋接層讀到相同內容");

  // packer 容量有限：分兩次拿要能銜接
  extent_list p1 = {0}, p2 = {0};
  p1.limit = 1;
  rc = ntfsb_blockmap(vol, mft, 0, 3 << 20, false, collect_extent, &p1);
  CHECK(rc == 0 && p1.count == 1, "blockmap 讀：packer 滿後停止 (rc=%d n=%d)", rc, p1.count);
  int64_t next = p1.count ? p1.items[0].logical + p1.items[0].length : 0;
  if (next < (3 << 20)) {
    rc = ntfsb_blockmap(vol, mft, next, (3 << 20) - next, false, collect_extent, &p2);
    CHECK(rc == 0 && p2.count > 0 && p2.items[0].logical <= next, "blockmap 讀：續傳從 %lld 接上", (long long)next);
  } else {
    CHECK(true, "blockmap 讀：單一 extent 已涵蓋全部（不需續傳）");
  }

  // 刻意打碎的檔案：建立 A 與 D，讓實體位置在前面的那個長到跨過後面那個的起點，
  // 配置器會被擋住而跳過去，於是它至少有兩段
  {
    uint64_t fa = make_file(vol, "koio_frag_a.bin", 1 << 20, 0xA0);
    uint64_t fd = make_file(vol, "koio_frag_d.bin", 1 << 20, 0xD0);
    CHECK(fa && fd, "blockmap 讀：建立 A 與 D");
    extent_list ea = {0}, ed = {0};
    ntfsb_blockmap(vol, fa, 0, 1 << 20, false, collect_extent, &ea);
    ntfsb_blockmap(vol, fd, 0, 1 << 20, false, collect_extent, &ed);
    CHECK(ea.count == 1 && ed.count == 1, "blockmap 讀：A 與 D 各自連續");
    bool a_first = ea.items[0].physical < ed.items[0].physical;
    uint64_t lower = a_first ? fa : fd;
    int64_t gap = a_first ? ed.items[0].physical - ea.items[0].physical
                          : ea.items[0].physical - ed.items[0].physical;
    // 讓前面的檔案長到超過後面檔案的起點再多 1 MB
    int64_t target = gap + (1 << 20);
    uint8_t *chunk = malloc(1 << 20);
    memset(chunk, 0xA1, 1 << 20);
    bool grew = true;
    for (int64_t off = 1 << 20; off < target && grew; off += 1 << 20) {
      grew = ntfsb_write(vol, lower, off, chunk, 1 << 20) == (1 << 20);
    }
    free(chunk);
    CHECK(grew, "blockmap 讀：前面的檔案長到 %lld MB", (long long)(target >> 20));
    fa = lower;
    ntfsb_attr fattr;
    CHECK(ntfsb_getattr(vol, fa, &fattr) == 0, "blockmap 讀：碎檔屬性");
    int64_t flen = (int64_t)fattr.size;
    extent_list all = {0};
    rc = ntfsb_blockmap(vol, fa, 0, flen, false, collect_extent, &all);
    CHECK(rc == 0 && all.count >= 2, "blockmap 讀：碎檔有 %d 個 extent", all.count);
    CHECK(extents_cover(&all, 0, flen), "blockmap 讀：碎檔 extent 完整覆蓋");
    extent_list f1 = {0}, f2 = {0};
    f1.limit = 1;
    rc = ntfsb_blockmap(vol, fa, 0, flen, false, collect_extent, &f1);
    int64_t cont = f1.count ? f1.items[0].logical + f1.items[0].length : 0;
    CHECK(rc == 0 && f1.count == 1 && cont < flen, "blockmap 讀：碎檔 packer 滿後停在 %lld", (long long)cont);
    rc = ntfsb_blockmap(vol, fa, cont, flen - cont, false, collect_extent, &f2);
    CHECK(rc == 0 && f2.count >= 1 && f2.items[0].logical == cont, "blockmap 讀：碎檔續傳從 %lld 接上", (long long)cont);
    uint8_t *fg = malloc(flen);
    uint8_t *fw = malloc(flen);
    CHECK(kernel_read(dev, &all, 0, flen, fg) == 0 && ntfsb_read(vol, fa, 0, fw, flen) == flen &&
              memcmp(fg, fw, flen) == 0,
          "blockmap 讀：碎檔 kernel 與橋接層內容一致");
    free(fg);
    free(fw);
  }

  // 目錄不能 blockmap
  rc = ntfsb_blockmap(vol, NTFSB_ROOT_MFT_NO, 0, 4096, false, collect_extent, &l);
  CHECK(rc == -EISDIR, "blockmap 讀：目錄回 EISDIR (rc=%d)", rc);

  free(pattern);
  free(got);
  free(want);
}

/// 假 kernel：依 extent 直接把資料寫進映像檔
static int kernel_write(file_device *dev, const extent_list *l, int64_t offset, int64_t len, const uint8_t *in) {
  for (int i = 0; i < l->count; i++) {
    const ntfsb_extent *e = &l->items[i];
    if (e->zero_fill) {
      return -1;  // 寫入路徑不該拿到 ZeroFill
    }
    int64_t lo = e->logical > offset ? e->logical : offset;
    int64_t hi = (e->logical + e->length < offset + len) ? e->logical + e->length : offset + len;
    if (hi <= lo) {
      continue;
    }
    if (pwrite(dev->fd, in + (lo - offset), hi - lo, e->physical + (lo - e->logical)) != hi - lo) {
      return -1;
    }
  }
  return 0;
}

/// 用 use_handle 決定是否經由持久 handle 跑寫入流程
static void test_blockmap_write(ntfsb_volume *vol, file_device *dev, bool use_handle) {
  const char *tag = use_handle ? "有 handle" : "無 handle";
  char name[64];
  snprintf(name, sizeof(name), "koio_write_%d.bin", use_handle);
  uint64_t mft = make_file(vol, name, 1 << 20, 0x11);
  CHECK(mft != 0, "blockmap 寫（%s）：建立 1 MB 檔案", tag);
  if (!mft) {
    return;
  }
  ntfsb_file *f = NULL;
  if (use_handle) {
    int err = 0;
    f = ntfsb_file_open(vol, mft, &err);
    CHECK(f != NULL, "blockmap 寫（%s）：開啟 handle (err=%d)", tag, err);
  }

  // 在 offset 3 MB 寫 1 MB：[1 MB, 3 MB) 必須被補零，大小變 4 MB
  int64_t off = 3 << 20, len = 1 << 20;
  extent_list l = {0};
  int rc = ntfsb_blockmap(vol, mft, off, len, true, collect_extent, &l);
  CHECK(rc == 0 && l.count > 0, "blockmap 寫（%s）：rc=%d extents=%d", tag, rc, l.count);
  CHECK(extents_cover(&l, off, len), "blockmap 寫（%s）：extent 覆蓋寫入範圍", tag);
  ntfsb_attr a;
  CHECK(ntfsb_getattr(vol, mft, &a) == 0 && a.size == (4u << 20),
        "blockmap 寫（%s）：blockmap 後 size=%llu", tag, (unsigned long long)a.size);
  CHECK(a.init_size == (3u << 20), "blockmap 寫（%s）：init_size 推到 3 MB (%llu)", tag,
        (unsigned long long)a.init_size);

  uint8_t *pat = malloc(len);
  for (int64_t i = 0; i < len; i++) {
    pat[i] = (uint8_t)(0x80 + (i % 97));
  }
  CHECK(kernel_write(dev, &l, off, len, pat) == 0, "blockmap 寫（%s）：假 kernel 寫入", tag);
  rc = ntfsb_complete_io(vol, mft, off, len, true, false, use_handle);
  CHECK(rc == 0, "blockmap 寫（%s）：complete_io (rc=%d)", tag, rc);
  CHECK(ntfsb_getattr(vol, mft, &a) == 0 && a.init_size == (4u << 20) && a.size == (4u << 20),
        "blockmap 寫（%s）：complete 後 size=init=4 MB (init=%llu)", tag, (unsigned long long)a.init_size);

  // 讀回三段
  uint8_t *buf = malloc(1 << 20);
  int64_t r = ntfsb_read(vol, mft, 0, buf, 1 << 20);
  CHECK(r == (1 << 20) && buf[0] == 0x11 && buf[(1 << 20) - 1] == 0x11, "blockmap 寫（%s）：原始段仍在", tag);
  r = ntfsb_read(vol, mft, 1 << 20, buf, 1 << 20);
  bool zero = (r == (1 << 20));
  for (int64_t i = 0; zero && i < r; i += 4093) {
    zero = buf[i] == 0;
  }
  CHECK(zero, "blockmap 寫（%s）：補零段是零", tag);
  r = ntfsb_read(vol, mft, 3 << 20, buf, 1 << 20);
  CHECK(r == (1 << 20) && memcmp(buf, pat, 1 << 20) == 0, "blockmap 寫（%s）：kernel 寫入的段讀回一致", tag);

  // 失敗的 I/O：size 拉長但 init 不動，讀回是零
  extent_list l2 = {0};
  rc = ntfsb_blockmap(vol, mft, 5 << 20, 1 << 20, true, collect_extent, &l2);
  CHECK(rc == 0, "blockmap 寫（%s）：失敗案例 blockmap (rc=%d)", tag, rc);
  CHECK(kernel_write(dev, &l2, 5 << 20, 1 << 20, pat) == 0, "blockmap 寫（%s）：失敗案例假 kernel 寫入", tag);
  rc = ntfsb_complete_io(vol, mft, 5 << 20, 1 << 20, true, true, use_handle);
  CHECK(rc == 0, "blockmap 寫（%s）：失敗案例 complete_io (rc=%d)", tag, rc);
  CHECK(ntfsb_getattr(vol, mft, &a) == 0 && a.size == (6u << 20) && a.init_size == (5u << 20),
        "blockmap 寫（%s）：失敗後 size=6 MB、init=5 MB (size=%llu init=%llu)", tag,
        (unsigned long long)a.size, (unsigned long long)a.init_size);
  r = ntfsb_read(vol, mft, 5 << 20, buf, 4096);
  CHECK(r == 4096 && buf[0] == 0 && buf[4095] == 0, "blockmap 寫（%s）：失敗段讀回零", tag);
  extent_list l3 = {0};
  rc = ntfsb_blockmap(vol, mft, 5 << 20, 4096, false, collect_extent, &l3);
  CHECK(rc == 0 && l3.count == 1 && l3.items[0].zero_fill, "blockmap 寫（%s）：失敗段對 kernel 也是 ZeroFill", tag);

  if (f) {
    CHECK(ntfsb_file_close(f) == 0, "blockmap 寫（%s）：關閉 handle", tag);
  }
  free(pat);
  free(buf);
}

/// write-back 快取一致性：慢路徑的小寫入必須在 blockmap 回覆前落盤
static void test_cache_coherency(ntfsb_volume *vol, file_device *dev) {
  uint64_t mft = make_file(vol, "koio_cache.bin", 64 << 10, 0x22);
  CHECK(mft != 0, "快取一致性：建立 64 KB 檔案");
  if (!mft) {
    return;
  }
  CHECK(ntfsb_sync(vol) == 0, "快取一致性：sync");
  // 8 KB 的慢路徑寫入會留在 write-back 快取裡
  uint8_t small[8192];
  memset(small, 0x33, sizeof(small));
  CHECK(ntfsb_write(vol, mft, 4096, small, sizeof(small)) == (int64_t)sizeof(small), "快取一致性：慢路徑寫 8 KB");
  extent_list l = {0};
  int rc = ntfsb_blockmap(vol, mft, 0, 64 << 10, false, collect_extent, &l);
  CHECK(rc == 0, "快取一致性：blockmap (rc=%d)", rc);
  uint8_t got[64 << 10];
  CHECK(kernel_read(dev, &l, 0, sizeof(got), got) == 0, "快取一致性：假 kernel 讀取");
  CHECK(got[4096] == 0x33 && got[4096 + 8191] == 0x33 && got[0] == 0x22,
        "快取一致性：kernel 看到慢路徑剛寫的資料（got[4096]=%02x）", got[4096]);
  // 反向：kernel 寫完後橋接層讀到新資料（快取不能留舊副本）
  uint8_t pat[4096];
  memset(pat, 0x44, sizeof(pat));
  extent_list lw = {0};
  rc = ntfsb_blockmap(vol, mft, 0, 4096, true, collect_extent, &lw);
  CHECK(rc == 0 && kernel_write(dev, &lw, 0, 4096, pat) == 0, "快取一致性：假 kernel 寫入前 4 KB");
  CHECK(ntfsb_complete_io(vol, mft, 0, 4096, true, false, false) == 0, "快取一致性：complete_io");
  uint8_t back[4096];
  CHECK(ntfsb_read(vol, mft, 0, back, sizeof(back)) == (int64_t)sizeof(back) && back[0] == 0x44 && back[4095] == 0x44,
        "快取一致性：橋接層讀到 kernel 寫的資料（back[0]=%02x）", back[0]);
}

int koio_test_run(ntfsb_volume *vol, file_device *dev) {
  failures = 0;
  printf("---- KOIO ----\n");
  test_eligibility();
  test_truncate_solid(vol);
  test_blockmap_read(vol, dev);
  test_blockmap_write(vol, dev, false);
  test_blockmap_write(vol, dev, true);
  test_cache_coherency(vol, dev);
  return failures;
}

int koio_test_verify_after_remount(ntfsb_volume *vol) {
  failures = 0;
  printf("---- KOIO（重掛後）----\n");
  uint64_t mft = 0;
  int rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "koio_write_1.bin", &mft, NULL);
  CHECK(rc == 0, "重掛：找到 koio_write_1.bin (rc=%d)", rc);
  if (rc) {
    return failures;
  }
  ntfsb_attr a;
  CHECK(ntfsb_getattr(vol, mft, &a) == 0 && a.size == (6u << 20) && a.init_size == (5u << 20),
        "重掛：size=6 MB init=5 MB (size=%llu init=%llu)",
        (unsigned long long)a.size, (unsigned long long)a.init_size);
  CHECK(ntfsb_koio_eligible(&a, CLUSTER), "重掛：仍合格");
  uint8_t *buf = malloc(1 << 20);
  int64_t r = ntfsb_read(vol, mft, 3 << 20, buf, 1 << 20);
  bool ok = (r == (1 << 20));
  for (int64_t i = 0; ok && i < r; i += 1021) {
    ok = buf[i] == (uint8_t)(0x80 + (i % 97));
  }
  CHECK(ok, "重掛：kernel 寫入的段內容正確");
  r = ntfsb_read(vol, mft, 1 << 20, buf, 4096);
  CHECK(r == 4096 && buf[0] == 0 && buf[4095] == 0, "重掛：補零段仍是零");
  free(buf);
  return failures;
}
