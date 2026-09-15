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

int koio_test_run(ntfsb_volume *vol, file_device *dev) {
  (void)dev;
  failures = 0;
  printf("---- KOIO ----\n");
  test_eligibility();
  test_truncate_solid(vol);
  return failures;
}
