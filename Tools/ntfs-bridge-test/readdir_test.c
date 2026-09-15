//
//  readdir_test.c
//  ntfs-bridge-test
//
//  大目錄的分段列舉。
//
//  項目一多，NTFS 的目錄索引就會從常駐的 $INDEX_ROOT 長成非常駐的
//  $INDEX_ALLOCATION（多個 INDX 區塊的 B-tree），cookie 的值域也隨之改變。
//  FSKit 會在封包填滿時停止並用 cookie 續傳，所以這裡照同樣方式驗證：
//  分段列舉的結果必須和一次列舉完全一致，不重複也不遺漏。
//

#include "readdir_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RDT_FILES  2000
#define RDT_CHUNK  50     ///< 模擬封包容量：每收這麼多項就停

typedef struct rdt_entry {
  char name[260];
} rdt_entry;

typedef struct rdt_ctx {
  rdt_entry *entries;
  int count;
  int cap;
  int limit;        ///< 收滿這麼多就回報停止；0 表示不限
  int64_t stop_cookie;
  int overflow;
} rdt_ctx;

static int rdt_cb(void *context, const char *name, size_t len, uint64_t mft,
                  ntfsb_type type, int64_t cookie, int64_t next) {
  (void)mft;
  (void)type;
  rdt_ctx *c = context;
  if (c->limit && c->count >= c->limit) {
    c->stop_cookie = cookie;
    return 1;  // 模擬封包已滿
  }
  if (c->count >= c->cap) {
    c->overflow = 1;
    return 1;
  }
  if (len >= sizeof(c->entries[0].name)) {
    len = sizeof(c->entries[0].name) - 1;
  }
  memcpy(c->entries[c->count].name, name, len);
  c->entries[c->count].name[len] = '\0';
  c->count++;
  (void)next;
  return 0;
}

static int rdt_cmp(const void *a, const void *b) {
  return strcmp(((const rdt_entry *)a)->name, ((const rdt_entry *)b)->name);
}

int readdir_test_run(ntfsb_volume *vol, uint64_t root_mft) {
  int failures = 0;
  const int cap = RDT_FILES + 64;

  uint64_t dir = 0;
  int rc = ntfsb_create(vol, root_mft, "列舉測試", NTFSB_TYPE_DIR, NULL, &dir);
  if (rc || dir < 16) {
    fprintf(stderr, "失敗：建立列舉測試目錄 (rc=%d)\n", rc);
    return 1;
  }
  for (int i = 0; i < RDT_FILES; i++) {
    char name[64];
    snprintf(name, sizeof(name), "e%04d.txt", i);
    uint64_t mft = 0;
    rc = ntfsb_create(vol, dir, name, NTFSB_TYPE_FILE, NULL, &mft);
    if (rc) {
      fprintf(stderr, "失敗：建立 %s (rc=%d)\n", name, rc);
      return 1;
    }
  }

  // ---- 一次列舉完 ----
  rdt_ctx full = { .entries = malloc(sizeof(rdt_entry) * (size_t)cap), .cap = cap };
  if (!full.entries) {
    fprintf(stderr, "失敗：配置列舉緩衝區\n");
    return 1;
  }
  int64_t pos = NTFSB_COOKIE_START;
  while (pos != NTFSB_COOKIE_END) {
    rc = ntfsb_readdir(vol, dir, &pos, rdt_cb, &full);
    if (rc) {
      fprintf(stderr, "失敗：一次列舉 (rc=%d，已收 %d 項)\n", rc, full.count);
      failures++;
      break;
    }
  }
  if (!failures) {
    printf("%s：一次列舉大目錄（收到 %d 項，含 . 與 ..）\n",
           full.count == RDT_FILES + 2 ? "通過" : "失敗", full.count);
    if (full.count != RDT_FILES + 2) {
      failures++;
    }
  }

  // ---- 分段列舉（模擬 FSKit 封包填滿後續傳）----
  rdt_ctx paged = { .entries = malloc(sizeof(rdt_entry) * (size_t)cap), .cap = cap };
  if (!paged.entries) {
    free(full.entries);
    return failures + 1;
  }
  pos = NTFSB_COOKIE_START;
  int rounds = 0;
  int64_t recent[4] = { -1, -1, -1, -1 };
  while (pos != NTFSB_COOKIE_END) {
    paged.limit = paged.count + RDT_CHUNK;   // 每輪最多再收 RDT_CHUNK 項
    paged.stop_cookie = -1;
    int before = paged.count;
    recent[rounds % 4] = pos;
    int64_t started_at = pos;
    rc = ntfsb_readdir(vol, dir, &pos, rdt_cb, &paged);
    rounds++;
    if (rc) {
      fprintf(stderr, "失敗：分段列舉第 %d 輪 (rc=%d，已收 %d 項)\n", rounds, rc, paged.count);
      fprintf(stderr, "      起始 cookie=%lld，結束 cookie=%lld\n",
              (long long)started_at, (long long)pos);
      for (int k = 0; k < 4; k++) {
        int idx = (rounds + k) % 4;
        if (recent[idx] >= 0) {
          fprintf(stderr, "      近期起始 cookie: %lld\n", (long long)recent[idx]);
        }
      }
      failures++;
      break;
    }
    if (paged.count == before && pos != NTFSB_COOKIE_END) {
      fprintf(stderr, "失敗：分段列舉第 %d 輪沒有進展（cookie=%lld，已收 %d 項）\n",
              rounds, (long long)pos, paged.count);
      failures++;
      break;
    }
    if (rounds > RDT_FILES + 100) {
      fprintf(stderr, "失敗：分段列舉輪數失控\n");
      failures++;
      break;
    }
  }
  if (paged.overflow) {
    fprintf(stderr, "失敗：分段列舉收到超出預期的項目數\n");
    failures++;
  }

  if (!failures) {
    if (paged.count != full.count) {
      fprintf(stderr, "失敗：分段列舉收到 %d 項，一次列舉收到 %d 項\n",
              paged.count, full.count);
      failures++;
    } else {
      qsort(full.entries, (size_t)full.count, sizeof(rdt_entry), rdt_cmp);
      qsort(paged.entries, (size_t)paged.count, sizeof(rdt_entry), rdt_cmp);
      int diff = -1;
      for (int i = 0; i < full.count; i++) {
        if (strcmp(full.entries[i].name, paged.entries[i].name) != 0) {
          diff = i;
          break;
        }
      }
      if (diff >= 0) {
        fprintf(stderr, "失敗：分段列舉內容不符，第 %d 項「%s」應為「%s」\n",
                diff, paged.entries[diff].name, full.entries[diff].name);
        failures++;
      } else {
        printf("通過：分段列舉大目錄與一次列舉一致（%d 項，%d 輪，每輪上限 %d）\n",
               paged.count, rounds, RDT_CHUNK);
      }
    }
  }

  free(full.entries);
  free(paged.entries);
  return failures;
}
