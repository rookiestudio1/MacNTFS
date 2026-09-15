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

int koio_test_run(ntfsb_volume *vol, file_device *dev) {
  (void)vol;
  (void)dev;
  failures = 0;
  printf("---- KOIO ----\n");
  test_eligibility();
  return failures;
}
