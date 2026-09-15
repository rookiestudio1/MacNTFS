# Kernel Offloaded I/O 實作計畫

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 讓一般檔案的資料 I/O 由 kernel 依延伸功能提供的 extent 直接對磁碟進行（FSKit KOIO），慢路徑只留給常駐／壓縮／加密／sparse 檔案。

**Architecture:** 橋接層新增 `ntfsb_koio_eligible`／`ntfsb_blockmap`／`ntfsb_complete_io`，在 `NTFSFairLock` 內解析 libntfs-3g runlist 並更新中繼資料；每個交出去的 extent 先對 `ntfs_blockcache` 做範圍 flush＋失效。`NTFSVolume` 加入 `FSVolumeKernelOffloadedIOOperations`，`getattr` 依規則設定 `inhibitKernelOffloadedIO`。測試由 `ntfs-bridge-test` 用映像檔扮演 kernel。

**Tech Stack:** C（libntfs-3g 2026.7.7）、Objective-C（FSKit）、XcodeGen、xcodebuild。

**Spec:** `docs/superpowers/specs/2026-09-15-kernel-offloaded-io-design.md`

## Global Constraints

- 程式碼註解一律繁體中文（台灣用詞）；縮排 2 個空白。
- 橋接層函式回傳 0／負 errno；讀寫類回傳位元組數或負 errno。
- libntfs-3g 與 `ntfs_blockcache` 都不是執行緒安全，所有橋接呼叫在 `NTFSFairLock` 內。
- 新增原始檔後必須 `xcodegen generate`，否則不會進 target。
- 建置：`xcodebuild -scheme ntfs-bridge-test -configuration Debug build`；測試：`Scripts/make-test-image.sh 256 /tmp/koio.img && build/Debug/ntfs-bridge-test /tmp/koio.img`。**每次測試都要用全新映像檔。**
- 端對端測試前必須先卸載所有由本延伸功能掛載的實體碟，否則 `fskitd` 會留下殘留狀態（`sudo killall fskitd` 才能復原）。
- 提交訊息結尾加 `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`。

---

## 檔案結構

| 檔案 | 責任 |
|---|---|
| `NTFSFSExtension/Bridge/ntfs_blockcache.{h,c}` | 新增 `ntfsb_bcache_flush_range` |
| `NTFSFSExtension/Bridge/ntfs_bridge.h` | 新增 `ntfsb_extent`、`ntfsb_extent_cb`、`ntfsb_koio_eligible`、`ntfsb_blockmap`、`ntfsb_complete_io`；`ntfsb_attr` 加 `resident`、`init_size` |
| `NTFSFSExtension/Bridge/ntfs_bridge.c` | 上述實作；`ntfsb_getattr` 填新欄位；`ntfsb_truncate` 改 `truncate_solid` |
| `Tools/ntfs-bridge-test/koio_test.{h,c}` | KOIO 橋接層測試（假 kernel） |
| `Tools/ntfs-bridge-test/main.c` | 接上 `koio_test_run` |
| `Tools/ntfs-bridge-test/bench.c` | 新增 KOIO 循序讀寫項目 |
| `NTFSFSExtension/NTFSVolume.{h,m}` | 遵循 `FSVolumeKernelOffloadedIOOperations`；四個新方法；`getattr` 規則 |
| `project.yml` | `FSSupportsKernelOffloadedIO: true` |
| `README.md` | 更新限制說明與 truncate 行為 |

---

### Task 1：`ntfsb_bcache_flush_range`

**Files:**
- Modify: `NTFSFSExtension/Bridge/ntfs_blockcache.h`（`ntfsb_bcache_flush` 之後）
- Modify: `NTFSFSExtension/Bridge/ntfs_blockcache.c`（`bc_invalidate_range` 之後、`ntfsb_bcache_flush` 之後）
- Test: `Tools/ntfs-bridge-test/bcache_test.c`

**Interfaces:**
- Produces: `int ntfsb_bcache_flush_range(ntfsb_bcache *c, int64_t offset, int64_t count);` 成功 0，失敗 -1 並設 errno。

- [ ] **Step 1: 看 bcache_test.c 現有的假裝置與測試樣式**

Run: `sed -n 1,80p Tools/ntfs-bridge-test/bcache_test.c`
找出它的記憶體假裝置（回呼 struct 與統計）與 `CHECK` 巨集的用法，新測試沿用同一套。

- [ ] **Step 2: 寫失敗的測試**

在 `bcache_test_run` 尾端（回傳前）加入（變數名依 Step 1 看到的假裝置調整）：

```c
  // ---- flush_range：範圍內髒行落盤且失效，範圍外髒行保持不動 ----
  {
    ntfsb_bcache *c = ntfsb_bcache_create(&cb, dev_size);
    uint8_t a[4096], b[4096], out[4096];
    memset(a, 0xA1, sizeof(a));
    memset(b, 0xB2, sizeof(b));
    // 行 3 與行 10 各寫一行（都留在快取裡）
    CHECK(ntfsb_bcache_pwrite(c, a, 4096, 3 * 4096) == 4096, "flush_range：寫入行 3");
    CHECK(ntfsb_bcache_pwrite(c, b, 4096, 10 * 4096) == 4096, "flush_range：寫入行 10");
    uint64_t writes_before = fake.write_calls;
    CHECK(ntfsb_bcache_flush_range(c, 3 * 4096, 4096) == 0, "flush_range：行 3");
    CHECK(fake.write_calls == writes_before + 1, "flush_range：只寫回一行 (%llu)",
          (unsigned long long)(fake.write_calls - writes_before));
    CHECK(memcmp(fake.data + 3 * 4096, a, 4096) == 0, "flush_range：行 3 已在裝置上");
    CHECK(memcmp(fake.data + 10 * 4096, b, 4096) != 0, "flush_range：行 10 仍未落盤");
    // 失效：直接改裝置內容後再經快取讀，必須看到新內容
    memset(fake.data + 3 * 4096, 0xC3, 4096);
    CHECK(ntfsb_bcache_pread(c, out, 4096, 3 * 4096) == 4096 && out[0] == 0xC3,
          "flush_range：行 3 已失效，重讀拿到裝置新內容");
    // 超大範圍（超過快取行數）也要正確，且不會逐行掃到天荒地老
    CHECK(ntfsb_bcache_flush_range(c, 0, dev_size) == 0, "flush_range：整個裝置");
    CHECK(memcmp(fake.data + 10 * 4096, b, 4096) == 0, "flush_range：行 10 這次落盤了");
    ntfsb_bcache_free(c);
  }
```

- [ ] **Step 3: 建置確認編譯失敗（符號不存在）**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD'`
Expected: `error: implicit declaration of function 'ntfsb_bcache_flush_range'`

- [ ] **Step 4: 實作**

`ntfs_blockcache.h`，`ntfsb_bcache_flush` 宣告之後：

```c
/// 把 [offset, offset+count) 涵蓋的髒行寫回，並讓範圍內所有行（髒或乾淨）失效。
/// 供 kernel offloaded I/O 使用：交給 kernel 直接 I/O 的範圍，快取不能再持有任何副本。
/// offset 與 count 必須對齊 block_size。成功回傳 0，失敗回傳 -1 並設定 errno。
int ntfsb_bcache_flush_range(ntfsb_bcache *c, int64_t offset, int64_t count);
```

`ntfs_blockcache.c`，`ntfsb_bcache_flush` 之後：

```c
int ntfsb_bcache_flush_range(ntfsb_bcache *c, int64_t offset, int64_t count) {
  if (!c || offset < 0 || count < 0) {
    errno = EINVAL;
    return -1;
  }
  if (c->line_count == 0 || count == 0) {
    return 0;
  }
  int64_t first = offset >> c->line_shift;
  int64_t last = (offset + count - 1) >> c->line_shift;
  // 範圍比快取還大時逐槽掃比逐行查雜湊便宜（一個 10 GB 的 extent 有兩百多萬行）
  if (last - first + 1 <= c->line_count) {
    return bc_invalidate_range(c, offset, count, true);
  }
  for (int32_t s = 0; s < c->line_count; s++) {
    int64_t ln = c->lines[s].line_no;
    if (ln == BC_EMPTY || ln < first || ln > last) {
      continue;
    }
    if (bc_invalidate_range(c, ln << c->line_shift, c->line_size, true) != 0) {
      return -1;
    }
  }
  return 0;
}
```

- [ ] **Step 5: 建置並跑測試**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E 'flush_range|失敗'`
Expected: 所有 `flush_range` 項目「通過」，沒有「失敗」。

- [ ] **Step 6: Commit**

```bash
git add NTFSFSExtension/Bridge/ntfs_blockcache.h NTFSFSExtension/Bridge/ntfs_blockcache.c Tools/ntfs-bridge-test/bcache_test.c
git commit -m "feat: 區塊快取加入範圍 flush 與失效"
```

---

### Task 2：`ntfsb_attr` 新欄位與 `ntfsb_koio_eligible`

**Files:**
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.h`（`ntfsb_attr` 結構；「MARK: - 檔案與目錄」之前新增「MARK: - Kernel offloaded I/O」段落）
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.c`（`ntfsb_getattr` 816–882 行；檔尾新增實作）
- Create: `Tools/ntfs-bridge-test/koio_test.h`、`Tools/ntfs-bridge-test/koio_test.c`
- Modify: `Tools/ntfs-bridge-test/main.c`

**Interfaces:**
- Produces:
  - `ntfsb_attr` 新增 `bool resident; uint64_t init_size;`
  - `bool ntfsb_koio_eligible(const ntfsb_attr *attr, uint32_t cluster_size);`
  - `int koio_test_run(ntfsb_volume *vol, file_device *dev);`（回傳失敗數）

- [ ] **Step 1: 建立測試檔骨架與合格規則的單元測試**

`Tools/ntfs-bridge-test/koio_test.h`：

```c
//
//  koio_test.h
//  ntfs-bridge-test
//

#ifndef KOIO_TEST_H
#define KOIO_TEST_H

#include "file_device.h"
#include "ntfs_bridge.h"

/// Kernel offloaded I/O 的橋接層測試：用 file_device 直接讀寫映像檔扮演 kernel。
/// 回傳失敗項目數。
int koio_test_run(ntfsb_volume *vol, file_device *dev);

#endif /* KOIO_TEST_H */
```

`Tools/ntfs-bridge-test/koio_test.c`：

```c
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
```

`main.c`：`#include "koio_test.h"`，並在 `failures += readdir_test_run(vol, NTFSB_ROOT_MFT_NO);` 之後加 `failures += koio_test_run(vol, &dev);`。

- [ ] **Step 2: xcodegen 與建置，確認失敗**

Run: `xcodegen generate >/dev/null && xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD'`
Expected: `error: no member named 'resident'`／`implicit declaration of function 'ntfsb_koio_eligible'`

- [ ] **Step 3: 標頭**

`ntfs_bridge.h` 的 `ntfsb_attr`，在 `bool sparse;` 之後加：

```c
  bool resident;        ///< $DATA 為常駐（資料在 MFT record 內，沒有叢集）
  uint64_t init_size;   ///< initialized_size：之後到 size 之前的範圍在磁碟上是垃圾，必須讀成零
```

在「// MARK: - 檔案與目錄」之前加：

```c
// MARK: - Kernel offloaded I/O
//
// FSKit 的 KOIO 讓 kernel 拿著檔案的邏輯→實體 extent 直接對裝置 I/O，
// 橋接層只負責：判定哪些檔案能這樣做、解析 runlist、在 I/O 完成後更新大小與時間戳。
// 交出去的叢集之後不能再經由區塊快取碰它，所以 ntfsb_blockmap 會先把範圍 flush 並失效。

/// 依檔案屬性判定是否可走 KOIO：常駐、壓縮、加密、sparse 不行；
/// initialized_size < size 且 initialized_size 不在叢集邊界也不行（半滿叢集的尾巴是垃圾）。
bool ntfsb_koio_eligible(const ntfsb_attr *attr, uint32_t cluster_size);
```

- [ ] **Step 4: 實作**

`ntfsb_getattr` 的 `else` 分支（一般檔案）改為：

```c
  } else {
    out->size = (uint64_t)ni->data_size;
    out->alloc_size = (uint64_t)ni->allocated_size;
    if (na && out->type == NTFSB_TYPE_SYMLINK) {
      // Interix 符號連結：大小為目標長度
      out->size = (uint64_t)(na->data_size - offsetof(INTX_FILE, target)) / sizeof(ntfschar);
    }
    if (out->type == NTFSB_TYPE_FILE) {
      // KOIO 合格判定需要 $DATA 是否常駐與 initialized_size；有 handle 就借用它的 attr
      ntfsb_file *held = ntfsb_find_file(v, mft_no);
      ntfs_attr *da = held ? held->na : (na ? na : ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0));
      if (da) {
        out->resident = !NAttrNonResident(da);
        out->init_size = (uint64_t)da->initialized_size;
        if (da != held->na && da != na) {
          ntfs_attr_close(da);
        }
      } else {
        // 開不了 $DATA 的檔案不交給 kernel
        out->resident = true;
      }
    }
  }
```

注意 `held` 可能為 NULL，`da != held->na` 要寫成 `!(held && da == held->na)`；實際寫法：

```c
      bool borrowed = (held && da == held->na) || (da == na);
      if (da && !borrowed) {
        ntfs_attr_close(da);
      }
```

檔尾新增：

```c
// MARK: - Kernel offloaded I/O

bool ntfsb_koio_eligible(const ntfsb_attr *a, uint32_t cluster_size) {
  if (!a || a->type != NTFSB_TYPE_FILE || cluster_size == 0) {
    return false;
  }
  if (a->resident || a->compressed || a->encrypted || a->sparse) {
    return false;
  }
  if (a->init_size < a->size && (a->init_size % cluster_size) != 0) {
    return false;
  }
  return true;
}
```

- [ ] **Step 5: 建置並跑測試**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E '合格|失敗'`
Expected: 10 個合格／不合格項目全部通過。

- [ ] **Step 6: Commit**

```bash
git add NTFSFSExtension/Bridge/ntfs_bridge.h NTFSFSExtension/Bridge/ntfs_bridge.c Tools/ntfs-bridge-test/koio_test.h Tools/ntfs-bridge-test/koio_test.c Tools/ntfs-bridge-test/main.c MacNTFS.xcodeproj
git commit -m "feat: 橋接層加入 KOIO 合格判定與 resident/init_size 屬性"
```

---

### Task 3：`ntfsb_truncate` 拉長改為真的配置叢集

**Files:**
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.c`（`ntfsb_truncate` 1448–1512 行）
- Test: `Tools/ntfs-bridge-test/koio_test.c`

- [ ] **Step 1: 寫失敗的測試**

`koio_test.c` 新增（放在 `test_eligibility` 之後）：

```c
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
```

在 `koio_test_run` 的 `test_eligibility();` 後加 `test_truncate_solid(vol);`。

- [ ] **Step 2: 建置並跑，確認「沒有變成 sparse」失敗**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E 'truncate_solid|失敗'`
Expected: 「沒有變成 sparse」與「仍合格」失敗，其餘通過。

- [ ] **Step 3: 實作**

`ntfsb_truncate` 內把

```c
    } else if (ntfs_attr_truncate(na, size)) {
```

改為

```c
    } else if (size > na->data_size && !(na->data_flags & ATTR_IS_SPARSE)
               ? ntfs_attr_truncate_solid(na, size)
               : ntfs_attr_truncate(na, size)) {
      // 拉長時真的配置叢集（與 Windows SetEndOfFile 一致）而不是留洞：
      // 留洞會把檔案標成 sparse，之後就不能走 kernel offloaded I/O。
      // 本來就是 sparse 的檔案維持原本留洞的行為。
```

- [ ] **Step 4: 建置並跑測試**

Run: 同 Step 2。
Expected: `truncate_solid` 全部通過；整體無「失敗」。

- [ ] **Step 5: Commit**

```bash
git add NTFSFSExtension/Bridge/ntfs_bridge.c Tools/ntfs-bridge-test/koio_test.c
git commit -m "fix: truncate 拉長時配置叢集而不留洞，避免檔案變成 sparse"
```

---

### Task 4：`ntfsb_blockmap`（讀取）

**Files:**
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.h`（KOIO 段落）
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.c`（檔尾 KOIO 段落）
- Test: `Tools/ntfs-bridge-test/koio_test.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct ntfsb_extent { int64_t logical; int64_t physical; int64_t length; bool zero_fill; } ntfsb_extent;
  typedef int (*ntfsb_extent_cb)(void *context, const ntfsb_extent *extent);
  int ntfsb_blockmap(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, int64_t length,
                     bool for_write, ntfsb_extent_cb cb, void *context);
  ```
- Consumes: Task 1 `ntfsb_bcache_flush_range`；`ntfsb_dev_priv.cache`（經 `v->vol->dev->d_private`）。

- [ ] **Step 1: 寫失敗的測試**

`koio_test.c` 新增：

```c
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
  CHECK(l.items[l.count - 1].zero_fill, "blockmap 讀：超過 init_size 的部分是 ZeroFill");

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
  int64_t next = p1.items[0].logical + p1.items[0].length;
  if (next < (3 << 20)) {
    rc = ntfsb_blockmap(vol, mft, next, (3 << 20) - next, false, collect_extent, &p2);
    CHECK(rc == 0 && p2.count > 0 && p2.items[0].logical <= next, "blockmap 讀：續傳從 %lld 接上", (long long)next);
  } else {
    CHECK(true, "blockmap 讀：單一 extent 已涵蓋全部（不需續傳）");
  }

  // 目錄不能 blockmap
  rc = ntfsb_blockmap(vol, NTFSB_ROOT_MFT_NO, 0, 4096, false, collect_extent, &l);
  CHECK(rc == -EISDIR, "blockmap 讀：目錄回 EISDIR (rc=%d)", rc);

  free(pattern);
  free(got);
  free(want);
}
```

`koio_test_run` 加 `test_blockmap_read(vol, dev);`。

- [ ] **Step 2: 建置確認編譯失敗**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD'`
Expected: `unknown type name 'ntfsb_extent'`

- [ ] **Step 3: 標頭**

`ntfs_bridge.h` KOIO 段落，`ntfsb_koio_eligible` 之後：

```c
/// 一段邏輯→實體對應。zero_fill 為 true 時 physical 無意義，該範圍讀成零。
typedef struct ntfsb_extent {
  int64_t logical;   ///< 檔案內位移（位元組）
  int64_t physical;  ///< 裝置位移（位元組，與裝置回呼同座標系）
  int64_t length;
  bool zero_fill;
} ntfsb_extent;

/// extent 回呼。回傳非 0 表示 packer 已滿，停止打包（不算錯誤）。
typedef int (*ntfsb_extent_cb)(void *context, const ntfsb_extent *extent);

/// 把檔案 [offset, offset+length) 依 runlist 打包成 extent，逐一交給 cb。
/// - 讀取：[0, initialized_size) 內是 Data，之後是 ZeroFill。
/// - 寫入（for_write）：先確保 [offset, offset+length) 已配置叢集、[initialized_size, offset) 已補零，
///   再打包 Data；唯讀卷回 -EROFS。
/// 每個 Data extent 交出去前，區塊快取裡對應的範圍會先 flush 並失效。
/// 目錄回 -EISDIR、非一般檔案回 -EINVAL。
int ntfsb_blockmap(ntfsb_volume *vol,
                   uint64_t mft_no,
                   int64_t offset,
                   int64_t length,
                   bool for_write,
                   ntfsb_extent_cb cb,
                   void *context);
```

- [ ] **Step 4: 實作（先只做讀取；for_write 先回 -EROFS 佔位）**

`ntfs_bridge.c` 檔尾 KOIO 段落：

```c
/// 取得裝置層的區塊快取（供 blockmap 做範圍 flush）
static ntfsb_bcache *ntfsb_volume_bcache(ntfsb_volume *v) {
  ntfsb_dev_priv *p = ntfsb_dev(v->vol->dev);
  return p ? p->cache : NULL;
}

/// 把 [start, end) 依 runlist 打包成 Data／ZeroFill extent。
/// 回傳 0、1（packer 已滿）或負 errno。
static int ntfsb_pack_runs(ntfsb_volume *v, ntfs_attr *na, int64_t start, int64_t end,
                           ntfsb_extent_cb cb, void *context) {
  ntfs_volume *vol = v->vol;
  ntfsb_bcache *cache = ntfsb_volume_bcache(v);
  if (ntfs_attr_map_whole_runlist(na)) {
    return ntfsb_neg_errno();
  }
  // 邏輯範圍對齊到叢集；kernel 自己會裁掉多出來的部分
  int64_t cs = vol->cluster_size;
  int64_t pos = start - (start % cs);
  int64_t stop = ((end + cs - 1) / cs) * cs;
  for (runlist_element *rl = na->rl; rl && rl->length > 0 && pos < stop; rl++) {
    int64_t run_start = rl->vcn * cs;
    int64_t run_end = run_start + rl->length * cs;
    if (run_end <= pos) {
      continue;
    }
    if (run_start > pos) {
      // runlist 有缺口（不該發生），保守地以零呈現
      ntfsb_extent gap = {pos, 0, run_start - pos, true};
      if (gap.logical + gap.length > stop) {
        gap.length = stop - gap.logical;
      }
      if (cb(context, &gap)) {
        return 1;
      }
      pos += gap.length;
      if (pos >= stop) {
        break;
      }
    }
    int64_t seg_end = run_end < stop ? run_end : stop;
    ntfsb_extent e = {pos, 0, seg_end - pos, false};
    if (rl->lcn == LCN_HOLE) {
      e.zero_fill = true;
    } else if (rl->lcn < 0) {
      return -EIO;
    } else {
      e.physical = rl->lcn * cs + (pos - run_start);
      // 交給 kernel 之前，快取不能再持有這段範圍的任何副本
      if (cache && ntfsb_bcache_flush_range(cache, e.physical, e.length) != 0) {
        return ntfsb_neg_errno();
      }
    }
    if (cb(context, &e)) {
      return 1;
    }
    pos = seg_end;
  }
  return 0;
}

int ntfsb_blockmap(ntfsb_volume *v, uint64_t mft_no, int64_t offset, int64_t length,
                   bool for_write, ntfsb_extent_cb cb, void *context) {
  if (!v || !v->vol || !cb || offset < 0 || length < 0) {
    return -EINVAL;
  }
  if (length == 0) {
    return 0;
  }
  if (for_write && NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    res = -EISDIR;
    goto exit;
  }
  if ((ni->flags & FILE_ATTR_REPARSE_POINT) || ni->mft_no < FILE_first_user) {
    res = -EINVAL;
    goto exit;
  }
  {
    // 有 handle 時必須沿用 handle 的 ntfs_attr，否則兩份 runlist 會分歧
    ntfsb_file *held = ntfsb_find_file(v, mft_no);
    ntfs_attr *na = held ? held->na : ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    if (!NAttrNonResident(na) || (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED))) {
      res = -EINVAL;
    } else if (for_write) {
      res = -EROFS;  // Task 5 補上
    } else {
      int64_t end = offset + length;
      int64_t init = na->initialized_size;
      int rc = 0;
      if (offset < init) {
        rc = ntfsb_pack_runs(v, na, offset, end < init ? end : init, cb, context);
      }
      if (rc == 0 && end > init) {
        // 超過 initialized_size 的部分在磁碟上是垃圾，交給 kernel 當零讀
        int64_t cs = v->vol->cluster_size;
        int64_t zstart = ((init + cs - 1) / cs) * cs;
        if (zstart < end) {
          ntfsb_extent z = {zstart, 0, end - zstart, true};
          rc = cb(context, &z) ? 1 : 0;
        }
      }
      res = rc < 0 ? rc : 0;
    }
    if (!held) {
      ntfs_attr_close(na);
    }
  }
exit:
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}
```

注意：`ntfsb_dev()` 定義在 232 行、`ntfsb_dev_priv` 在 109 行，都在此段落之前，可直接用。

- [ ] **Step 5: 建置並跑測試**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E 'blockmap 讀|失敗'`
Expected: `blockmap 讀` 全部通過。

- [ ] **Step 6: Commit**

```bash
git add NTFSFSExtension/Bridge/ntfs_bridge.h NTFSFSExtension/Bridge/ntfs_bridge.c Tools/ntfs-bridge-test/koio_test.c
git commit -m "feat: 橋接層加入 KOIO 讀取 blockmap"
```

---

### Task 5：`ntfsb_blockmap`（寫入）與 `ntfsb_complete_io`

**Files:**
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.h`（KOIO 段落）
- Modify: `NTFSFSExtension/Bridge/ntfs_bridge.c`（KOIO 段落）
- Test: `Tools/ntfs-bridge-test/koio_test.c`

**Interfaces:**
- Produces: `int ntfsb_complete_io(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, int64_t length, bool for_write, bool io_failed, bool defer_meta);`

- [ ] **Step 1: 寫失敗的測試**

`koio_test.c` 新增：

```c
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
```

`koio_test_run` 加：

```c
  test_blockmap_write(vol, dev, false);
  test_blockmap_write(vol, dev, true);
  test_cache_coherency(vol, dev);
```

- [ ] **Step 2: 建置確認編譯失敗**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD'`
Expected: `implicit declaration of function 'ntfsb_complete_io'`

- [ ] **Step 3: 標頭**

`ntfs_bridge.h` KOIO 段落，`ntfsb_blockmap` 之後：

```c
/// kernel 完成一次 I/O 後更新中繼資料。
/// - 寫入成功：data_size 與 initialized_size 推進到 offset+length、更新 mtime／ctime。
/// - 寫入失敗（io_failed）：不推進 initialized_size，該範圍在兩條路徑上都讀成零。
/// - 讀取：不做事。
/// defer_meta 為 true 且檔案有 handle 時，MFT record 交給定時 flush；否則立即寫回。
int ntfsb_complete_io(ntfsb_volume *vol,
                      uint64_t mft_no,
                      int64_t offset,
                      int64_t length,
                      bool for_write,
                      bool io_failed,
                      bool defer_meta);
```

- [ ] **Step 4: 實作**

在 `ntfsb_pack_runs` 之前加輔助函式：

```c
/// 把 na 的 initialized_size 設為 size（記憶體與屬性記錄一起改）。
/// libntfs-3g 沒有公開這個操作；做法與 ntfs_attr_pwrite 尾端相同。
static int ntfsb_attr_set_initialized(ntfs_attr *na, int64_t size) {
  if (size < 0 || size > na->data_size) {
    errno = EINVAL;
    return -1;
  }
  if (size == na->initialized_size) {
    return 0;
  }
  ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(na->ni, NULL);
  if (!ctx) {
    return -1;
  }
  if (ntfs_attr_lookup(na->type, na->name, na->name_len, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
    ntfs_attr_put_search_ctx(ctx);
    return -1;
  }
  na->initialized_size = size;
  ctx->attr->initialized_size = cpu_to_sle64(size);
  ntfs_inode_mark_dirty(ctx->ntfs_ino);
  ntfs_attr_put_search_ctx(ctx);
  return 0;
}

/// 把 [na->initialized_size, upto) 補零（會經由 ntfs_attr_pwrite 推進 initialized_size）。
/// 用 >= 64 KB 的區塊寫，讓大部分繞過區塊快取直寫裝置。
static int ntfsb_attr_zero_to(ntfs_attr *na, int64_t upto) {
  enum { ZERO_CHUNK = 256 * 1024 };
  if (upto <= na->initialized_size) {
    return 0;
  }
  uint8_t *zeros = calloc(1, ZERO_CHUNK);
  if (!zeros) {
    errno = ENOMEM;
    return -1;
  }
  int res = 0;
  while (na->initialized_size < upto) {
    int64_t pos = na->initialized_size;
    int64_t n = upto - pos;
    if (n > ZERO_CHUNK) {
      n = ZERO_CHUNK;
    }
    s64 w = ntfs_attr_pwrite(na, pos, n, zeros);
    if (w <= 0) {
      res = -1;
      break;
    }
  }
  free(zeros);
  return res;
}
```

`ntfsb_blockmap` 內把 `res = -EROFS;  // Task 5 補上` 換成：

```c
      int64_t end = offset + length;
      int rc = 0;
      // 1. 確保 [offset, end) 有叢集：拉長時真的配置，不留洞
      if (end > na->data_size && ntfs_attr_truncate_solid(na, end)) {
        rc = ntfsb_neg_errno();
      }
      // 2. kernel 只會寫 [offset, end)，中間的洞要先在磁碟上補零
      if (!rc && offset > na->initialized_size && ntfsb_attr_zero_to(na, offset)) {
        rc = ntfsb_neg_errno();
      }
      // 3. 打包
      if (!rc) {
        rc = ntfsb_pack_runs(v, na, offset, end, cb, context);
      }
      if (!rc || rc == 1) {
        ni->flags |= FILE_ATTR_ARCHIVE;
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
        if (held) {
          held->meta_dirty = true;
        }
      }
      res = rc < 0 ? rc : 0;
```

並在 `ntfsb_blockmap` 之後加：

```c
int ntfsb_complete_io(ntfsb_volume *v, uint64_t mft_no, int64_t offset, int64_t length,
                      bool for_write, bool io_failed, bool defer_meta) {
  if (!v || !v->vol || offset < 0 || length < 0) {
    return -EINVAL;
  }
  if (!for_write) {
    return 0;  // 讀取不更新 atime
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    res = -EISDIR;
    goto exit;
  }
  {
    ntfsb_file *held = ntfsb_find_file(v, mft_no);
    ntfs_attr *na = held ? held->na : ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    int64_t end = offset + length;
    if (!io_failed) {
      if (end > na->data_size && ntfs_attr_truncate_solid(na, end)) {
        res = ntfsb_neg_errno();
      } else if (end > na->initialized_size && ntfsb_attr_set_initialized(na, end)) {
        res = ntfsb_neg_errno();
      }
    }
    // 失敗的寫入：data_size 已在 blockmap 拉長，initialized_size 不推進，
    // 於是 libntfs-3g 與 kernel（ZeroFill）讀到的都是零，兩邊一致。
    if (!held) {
      ntfs_attr_close(na);
    }
    if (!res) {
      ni->flags |= FILE_ATTR_ARCHIVE;
      ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
      if (held) {
        held->meta_dirty = true;
        if (!defer_meta && ntfsb_file_flush(held)) {
          res = ntfsb_neg_errno();
        }
      }
      // 沒有 handle 時 ntfsb_close_inode 會真的關閉並寫回 MFT record
    }
  }
exit:
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}
```

`ntfs_attr_search_ctx` 的 inode 欄位名稱請以 `grep -n 'ntfs_ino' ThirdParty/ntfs-3g/include/ntfs-3g/attrib.h` 確認（預期是 `ntfs_ino`）。

- [ ] **Step 5: 建置並跑測試**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E 'blockmap 寫|快取一致性|失敗'`
Expected: 全部通過。若「init_size 推到 3 MB」失敗，檢查 `ntfs_attr_pwrite` 補零時是否把 init 推到 `offset`（它會推到最後一次 pwrite 的結尾）。

- [ ] **Step 6: 跑完整測試（確認舊測試沒壞）**

Run: `Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -c '失敗'`
Expected: `0`

- [ ] **Step 7: Commit**

```bash
git add NTFSFSExtension/Bridge/ntfs_bridge.h NTFSFSExtension/Bridge/ntfs_bridge.c Tools/ntfs-bridge-test/koio_test.c
git commit -m "feat: 橋接層加入 KOIO 寫入 blockmap 與 complete_io"
```

---

### Task 6：卸載重掛後的持久性驗證

**Files:**
- Modify: `Tools/ntfs-bridge-test/main.c`（測試流程末端）
- Test: `Tools/ntfs-bridge-test/koio_test.h`、`koio_test.c`

**Interfaces:**
- Produces: `int koio_test_verify_after_remount(ntfsb_volume *vol);`（回傳失敗數）

- [ ] **Step 1: 看 main.c 現有的卸載／重掛流程**

Run: `grep -n 'ntfsb_umount\|ntfsb_mount' Tools/ntfs-bridge-test/main.c`
若已有重掛段落，新測試接在它之後；若沒有，在最後的 `ntfsb_umount` 之後加一次 `ntfsb_mount` 再呼叫新函式，最後再 `ntfsb_umount`。

- [ ] **Step 2: 寫測試**

`koio_test.h` 加：

```c
/// 卸載重掛後檢查 KOIO 寫入的檔案內容與大小仍正確。回傳失敗項目數。
int koio_test_verify_after_remount(ntfsb_volume *vol);
```

`koio_test.c` 加：

```c
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
```

`main.c` 依 Step 1 接上呼叫。

- [ ] **Step 3: 建置並跑**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 256 /tmp/koio.img >/dev/null && build/Debug/ntfs-bridge-test /tmp/koio.img 2>&1 | grep -E '重掛|失敗'`
Expected: 全部通過。

- [ ] **Step 4: Commit**

```bash
git add Tools/ntfs-bridge-test/main.c Tools/ntfs-bridge-test/koio_test.h Tools/ntfs-bridge-test/koio_test.c
git commit -m "test: KOIO 寫入結果在卸載重掛後仍正確"
```

---

### Task 7：bench 加入 KOIO 項目

**Files:**
- Modify: `Tools/ntfs-bridge-test/bench.c`

- [ ] **Step 1: 看 bench 的項目結構**

Run: `sed -n 205,300p Tools/ntfs-bridge-test/bench.c`
找出 `bench_result` 欄位、`print_result` 的用法，與「循序寫入」項目怎麼建檔與計時。

- [ ] **Step 2: 加入項目「KOIO 循序寫」與「KOIO 循序讀」**

在既有「循序覆寫」項目之後加（變數名依 Step 1 調整；核心是計 blockmap 呼叫次數與裝置 I/O 次數）：

```c
  // ---- KOIO 循序寫：kernel 以 1 MB 為單位 blockmap→pwrite→complete_io ----
  {
    const int64_t total = 256 << 20, chunk = 1 << 20;
    uint64_t mft = 0;
    ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "koio_seq.bin", NTFSB_TYPE_FILE, NULL, &mft);
    int err = 0;
    ntfsb_file *f = ntfsb_file_open(vol, mft, &err);
    uint8_t *buf = malloc(chunk);
    memset(buf, 0x5A, chunk);
    file_device_reset_stats(&dev);
    uint64_t blockmaps = 0;
    double t0 = now();
    for (int64_t off = 0; off < total; off += chunk) {
      koio_extents l = {0};
      ntfsb_blockmap(vol, mft, off, chunk, true, koio_collect, &l);
      blockmaps++;
      for (int i = 0; i < l.count; i++) {
        pwrite(dev.fd, buf + (l.items[i].logical - off), l.items[i].length, l.items[i].physical);
      }
      ntfsb_complete_io(vol, mft, off, chunk, true, false, true);
    }
    ntfsb_file_close(f);
    ntfsb_sync(vol);
    double t1 = now();
    // 裝置 I/O 統計只涵蓋橋接層自己的（中繼資料與補零）；假 kernel 的 pwrite 不經 file_device
    print_line("KOIO 循序寫", t1 - t0, total, dev.stats.read_calls, dev.stats.write_calls, blockmaps);
    free(buf);
  }
```

`koio_extents`／`koio_collect` 在 bench.c 內自行定義（與 koio_test.c 的 `extent_list`／`collect_extent` 相同，但 bench 不連結 koio_test.c）。`print_line` 依 Step 1 看到的輸出函式改寫，多印一欄 blockmap 次數。「KOIO 循序讀」同樣結構：`for_write=false`、`pread`、不呼叫 complete_io。

- [ ] **Step 3: 建置並跑 bench**

Run: `xcodebuild -scheme ntfs-bridge-test -configuration Debug build 2>&1 | grep -E 'error|BUILD' && Scripts/make-test-image.sh 512 /tmp/bench.img >/dev/null && build/Debug/ntfs-bridge-test bench /tmp/bench.img 2>&1 | grep -E 'KOIO|項目'`
Expected: 兩個 KOIO 項目有輸出；KOIO 循序讀的裝置讀取次數 ≤ 個位數（runlist 已在 handle 裡，只有第一次 map 需要讀 MFT）；KOIO 循序寫的裝置寫入次數遠小於 256（只有 MFT 與 $Bitmap）。把數字記在 commit 訊息裡。

- [ ] **Step 4: Commit**

```bash
git add Tools/ntfs-bridge-test/bench.c
git commit -m "test: bench 加入 KOIO 循序讀寫項目"
```

---

### Task 8：NTFSVolume 接上 KOIO（里程碑 1：只在唯讀掛載放行）

**Files:**
- Modify: `NTFSFSExtension/NTFSVolume.h`（協定清單）
- Modify: `NTFSFSExtension/NTFSVolume.m`（`attributesFromBridge:` 241 行；`activateWithOptions:` 386 行；新增「MARK: - FSVolumeKernelOffloadedIOOperations」段落）
- Modify: `project.yml`

- [ ] **Step 1: 協定與 plist**

`NTFSVolume.h` 的協定清單加 `FSVolumeKernelOffloadedIOOperations`（放在 `FSVolumeReadWriteOperations` 之後）。
`project.yml` 的 `FSSupportsKernelOffloadedIO: false` 改 `true`。執行 `xcodegen generate`。

- [ ] **Step 2: 快取叢集大小與放行開關**

`NTFSVolume.m` 的 class extension 加：

```objc
/// 掛載時從 statfs 取得，KOIO 合格判定用
@property (nonatomic) uint32_t clusterSize;
/// 里程碑 1：只在唯讀掛載放行 KOIO；讀寫掛載驗證完成後改為一律放行
@property (nonatomic) BOOL koioEnabled;
```

`activateWithOptions:` 在 `BOOL writable = !ntfsb_is_readonly(_vol);` 之後加：

```objc
  ntfsb_statfs_info sf;
  self.clusterSize = (ntfsb_statfs(_vol, &sf) == 0) ? sf.cluster_size : 0;
  self.koioEnabled = (self.clusterSize > 0) && !writable;
```

`attributesFromBridge:` 把 `attrs.inhibitKernelOffloadedIO = YES;` 改為：

```objc
  attrs.inhibitKernelOffloadedIO = !(self.koioEnabled && ntfsb_koio_eligible(a, self.clusterSize));
```

- [ ] **Step 3: 四個協定方法**

在 `// MARK: - FSVolumeXattrOperations` 之前加：

```objc
// MARK: - FSVolumeKernelOffloadedIOOperations

/// blockmap 回呼的上下文：把橋接層的 extent 轉交給 FSExtentPacker
typedef struct {
  __unsafe_unretained FSExtentPacker *packer;
  __unsafe_unretained FSBlockDeviceResource *resource;
} NTFSPackContext;

static int NTFSPackExtent(void *context, const ntfsb_extent *e) {
  NTFSPackContext *c = context;
  BOOL more = [c->packer packExtentWithResource:c->resource
                                           type:e->zero_fill ? FSExtentTypeZeroFill : FSExtentTypeData
                                  logicalOffset:e->logical
                                 physicalOffset:e->physical
                                         length:(size_t)e->length];
  return more ? 0 : 1;
}

- (void)blockmapFile:(FSItem *)file
              offset:(off_t)offset
              length:(size_t)length
               flags:(FSBlockmapFlags)flags
         operationID:(FSOperationID)operationID
              packer:(FSExtentPacker *)packer
        replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:file];
  if (!ni) {
    reply(NTFSError(EINVAL));
    return;
  }
  NTFSPackContext ctx = {packer, self.device.resource};
  [self.lock lock];
  int rc = _vol ? ntfsb_blockmap(_vol, ni.mftNo, offset, (int64_t)length,
                                 (flags & FSBlockmapFlagsWrite) != 0, NTFSPackExtent, &ctx)
                : -ENXIO;
  [self.lock unlock];
  if (rc) {
    os_log_error(NTFSVolumeLog(), "blockmap mft=%llu off=%lld len=%zu write=%d 失敗 rc=%d",
                 ni.mftNo, (long long)offset, length, (flags & FSBlockmapFlagsWrite) != 0, rc);
  }
  reply(rc ? NTFSError(rc) : nil);
}

- (void)completeIOForFile:(FSItem *)file
                   offset:(off_t)offset
                   length:(size_t)length
                   status:(NSError *)status
                    flags:(FSCompleteIOFlags)flags
              operationID:(FSOperationID)operationID
             replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:file];
  if (!ni) {
    reply(NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int rc = _vol ? ntfsb_complete_io(_vol, ni.mftNo, offset, (int64_t)length,
                                    (flags & FSCompleteIOFlagsWrite) != 0,
                                    status != nil,
                                    (flags & FSCompleteIOFlagsAsync) != 0)
                : -ENXIO;
  [self.lock unlock];
  reply(rc ? NTFSError(rc) : nil);
}

// 不機會性提供 extent：直接轉呼叫既有的建立與查找
- (void)createFileNamed:(FSFileName *)name
            inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)attributes
                 packer:(FSExtentPacker *)packer
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
  [self createItemNamed:name type:FSItemTypeFile inDirectory:directory attributes:attributes replyHandler:reply];
}

- (void)lookupItemNamed:(FSFileName *)name
            inDirectory:(FSItem *)directory
                 packer:(FSExtentPacker *)packer
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
  [self lookupItemNamed:name inDirectory:directory replyHandler:reply];
}
```

- [ ] **Step 4: 建置整個 App**

Run: `xcodebuild -scheme MacNTFS -configuration Debug build 2>&1 | grep -E 'error|warning: incomplete|BUILD'`
Expected: `BUILD SUCCEEDED`，沒有「incomplete implementation」警告。

- [ ] **Step 5: 端對端（唯讀）**

先確認實體 NTFS 碟都已卸載（`mount | grep ntfs3g` 應為空）。

```bash
Scripts/make-test-image.sh 256 /tmp/e2e.img
# 先用讀寫掛載放一個大檔進去（此時 KOIO 尚未放行）
DEV=$(hdiutil attach -nomount /tmp/e2e.img | awk 'NR==1{print $1}')
open build/Debug/MacNTFS.app; sleep 2
diskutil mount $DEV
MP=$(mount | grep "$DEV" | sed -E 's/.* on (.*) \(.*/\1/')
dd if=/dev/urandom of=/tmp/big.bin bs=1m count=64
cp /tmp/big.bin "$MP/big.bin"; md5 /tmp/big.bin "$MP/big.bin"
diskutil unmount $DEV
# 唯讀重掛：這次 KOIO 放行
diskutil mount readOnly $DEV
MP=$(mount | grep "$DEV" | sed -E 's/.* on (.*) \(.*/\1/')
md5 "$MP/big.bin"
log show --last 2m --predicate 'subsystem == "com.rookiestudio.MacNTFS"' | grep -i blockmap | head
diskutil unmount $DEV; hdiutil detach $DEV
```

Expected: md5 相同；log 若有 blockmap 錯誤要查明。用 `sudo fs_usage -w -f filesys | grep big.bin` 可看到 kernel 直接讀取（沒有經過延伸功能的 `readFromFile`）。

- [ ] **Step 6: Commit**

```bash
git add NTFSFSExtension/NTFSVolume.h NTFSFSExtension/NTFSVolume.m project.yml MacNTFS.xcodeproj NTFSFSExtension/Info.plist
git commit -m "feat: FSKit 層接上 kernel offloaded I/O（先只在唯讀掛載放行）"
```

---

### Task 9：里程碑 2：讀寫掛載放行與端對端驗證

**Files:**
- Modify: `NTFSFSExtension/NTFSVolume.m`（`activateWithOptions:` 的 `koioEnabled`）
- Modify: `README.md`

- [ ] **Step 1: 放行**

`self.koioEnabled = (self.clusterSize > 0) && !writable;` 改為 `self.koioEnabled = (self.clusterSize > 0);`，並把 class extension 裡 `koioEnabled` 的註解改為「叢集大小取得失敗時關閉 KOIO（合格判定需要它）」。

- [ ] **Step 2: 建置**

Run: `xcodebuild -scheme MacNTFS -configuration Debug build 2>&1 | grep -E 'error|BUILD'`
Expected: `BUILD SUCCEEDED`

- [ ] **Step 3: 端對端（讀寫）**

先確認實體碟都已卸載。用新映像檔：

```bash
Scripts/make-test-image.sh 512 /tmp/e2e.img
DEV=$(hdiutil attach -nomount /tmp/e2e.img | awk 'NR==1{print $1}')
open build/Debug/MacNTFS.app; sleep 2
diskutil mount $DEV
MP=$(mount | grep "$DEV" | sed -E 's/.* on (.*) \(.*/\1/')
# 1. 大檔來回
dd if=/dev/urandom of=/tmp/big.bin bs=1m count=128
cp /tmp/big.bin "$MP/big.bin"; cp "$MP/big.bin" /tmp/back.bin; md5 /tmp/big.bin /tmp/back.bin
# 2. 跳位寫入（補零）
dd if=/tmp/big.bin of="$MP/seek.bin" bs=1m count=1 seek=10 conv=notrunc
ls -l "$MP/seek.bin"; dd if="$MP/seek.bin" bs=1m count=10 2>/dev/null | od -A d -t x1 | head -2
# 3. ftruncate 後循序寫
python3 -c "import os; f=open('$MP/trunc.bin','wb'); f.truncate(32<<20); f.write(b'A'*(32<<20)); f.close()"
python3 -c "d=open('$MP/trunc.bin','rb').read(); print(len(d), d.count(b'A')==len(d))"
# 4. mmap 讀寫
python3 -c "
import mmap
f=open('$MP/big.bin','r+b'); m=mmap.mmap(f.fileno(),0); m[0:8]=b'MMAPTEST'; m.flush(); m.close(); f.close()
print(open('$MP/big.bin','rb').read(8))"
# 5. F_LOG2PHYS
python3 -c "
import fcntl,struct
f=open('$MP/big.bin','rb'); buf=struct.pack('IqQq',0,0,0,0)
print(fcntl.fcntl(f, 49, buf))"   # F_LOG2PHYS=49
# 6. 卸載重掛後再比一次
diskutil unmount $DEV; diskutil mount $DEV
MP=$(mount | grep "$DEV" | sed -E 's/.* on (.*) \(.*/\1/')
md5 /tmp/big.bin "$MP/big.bin"   # 前 8 bytes 已被 mmap 改掉，只看 seek/trunc 的正確性
python3 -c "d=open('$MP/seek.bin','rb').read(); print(len(d)==11<<20, d[:10<<20]==bytes(10<<20))"
diskutil unmount $DEV; hdiutil detach $DEV
```

Expected：1 的 md5 相同；2 顯示 11 MB、前 10 MB 全零；3 印 `33554432 True`；4 印 `b'MMAPTEST'`；5 不丟例外；6 兩個 True。

若步驟 2 或 3 讀到非零垃圾，代表 kernel 保留了舊的 ZeroFill 映射——啟用設計文件第 12 節的備案（Write blockmap 先把 `[init_size, offset+length)` 全部補零到磁碟），再重跑。

- [ ] **Step 4: 效能對照（實體碟，可選）**

用一顆實體 NTFS 碟，先 `git stash` 回到 KOIO 前的版本量一次、再回來量一次：

```bash
time cp /tmp/big.bin "/Volumes/<卷標>/big.bin"; time cp "/Volumes/<卷標>/big.bin" /tmp/back.bin
# 同時觀察：top -pid $(pgrep -f NTFSFSExtension) -l 3 | tail -1
```

把吞吐與 CPU 記進 commit 訊息。

- [ ] **Step 5: README**

`README.md` 175 行的「不支援 Kernel-offloaded I/O，大檔案吞吐量受 XPC 往返限制。」改為：

```markdown
- 一般檔案的資料 I/O 走 Kernel-offloaded I/O（kernel 依延伸功能提供的 extent 直接讀寫磁碟）；常駐（約 700 bytes 以下）、壓縮、加密、sparse 檔案仍經由延伸功能讀寫。
- 拉長檔案（`ftruncate`）會真的配置叢集，與 Windows 一致；不會建立 sparse 檔。
```

- [ ] **Step 6: Commit**

```bash
git add NTFSFSExtension/NTFSVolume.m README.md
git commit -m "feat: 讀寫掛載也放行 kernel offloaded I/O"
```

---

### Task 10：更新設計文件狀態與專案記憶

**Files:**
- Modify: `docs/superpowers/specs/2026-09-15-kernel-offloaded-io-design.md`（狀態列）
- Modify: `/Users/rookie/.claude/projects/-Volumes-External-HD-Works-macntfs/memory/macntfs-performance-roadmap.md`

- [ ] **Step 1: 設計文件狀態**

「狀態：設計已審閱，待撰寫實作計畫」改為「狀態：已依此實作完成；`ntfsb_koio_eligible` 的簽章實作時改為 `(const ntfsb_attr *, uint32_t cluster_size)`，不需要 volume」。若 Task 9 啟用了備案，把第 5 節 Write 步驟 2 改成實際行為。

- [ ] **Step 2: 專案記憶**

在「已完成」加第 6 項：kernel offloaded I/O（合格規則、blockmap／complete_io、blockcache flush_range、truncate_solid），附 bench 與端對端數字；「剩餘待辦」移除 KOIO 與「縮短持鎖時間」兩項。

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-09-15-kernel-offloaded-io-design.md
git commit -m "docs: KOIO 設計文件標記為已實作"
```
