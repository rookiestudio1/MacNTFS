//
//  ntfs_blockcache.c
//  NTFSFSExtension
//

#include "ntfs_blockcache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BC_EMPTY (-1)

/// 一個快取行的中繼資料。資料本身放在 slab 裡，索引即槽號。
typedef struct bcache_line {
  int64_t line_no;    ///< 行號（位移 / line_size）；BC_EMPTY 表示未使用
  int32_t hash_next;  ///< 雜湊鏈的下一個槽
  int32_t lru_prev;
  int32_t lru_next;
  bool dirty;
} bcache_line;

struct ntfsb_bcache {
  ntfsb_device_callbacks cb;
  int64_t size;         ///< 裝置總位元組數
  int64_t full_lines;   ///< 完整落在裝置內的行數（尾端不足一行的部分一律繞過）
  uint32_t line_size;
  uint32_t line_shift;  ///< line_size 的位元數，用位移取代除法
  int32_t line_count;   ///< 0 表示退化為純轉呼叫
  uint8_t *slab;
  bcache_line *lines;
  int32_t *buckets;
  int32_t bucket_mask;
  int32_t lru_head;     ///< 最近使用
  int32_t lru_tail;     ///< 最久未使用
  bool readonly;
};

// MARK: - 直接裝置存取（處理部分完成的回呼）

static int64_t bc_direct_pread(ntfsb_bcache *c, void *buf, int64_t count, int64_t offset) {
  int64_t total = 0;
  while (total < count) {
    int64_t got = c->cb.pread(c->cb.context, (uint8_t *)buf + total, count - total, offset + total);
    if (got < 0) {
      if (!errno) {
        errno = EIO;
      }
      return -1;
    }
    if (got == 0) {
      break;
    }
    total += got;
  }
  return total;
}

static int64_t bc_direct_pwrite(ntfsb_bcache *c, const void *buf, int64_t count, int64_t offset) {
  int64_t total = 0;
  while (total < count) {
    int64_t put = c->cb.pwrite(c->cb.context, (const uint8_t *)buf + total, count - total, offset + total);
    if (put < 0) {
      if (!errno) {
        errno = EIO;
      }
      return -1;
    }
    if (put == 0) {
      errno = ENOSPC;
      return -1;
    }
    total += put;
  }
  return total;
}

// MARK: - 雜湊表與 LRU

static inline uint8_t *bc_data(ntfsb_bcache *c, int32_t slot) {
  return c->slab + (size_t)slot * c->line_size;
}

static inline int32_t bc_bucket(const ntfsb_bcache *c, int64_t line_no) {
  // line_no 多半是連續的，乘上奇數常數再取高位以打散
  uint64_t h = (uint64_t)line_no * 0x9E3779B97F4A7C15ull;
  return (int32_t)((h >> 32) & (uint64_t)c->bucket_mask);
}

static int32_t bc_find(const ntfsb_bcache *c, int64_t line_no) {
  for (int32_t s = c->buckets[bc_bucket(c, line_no)]; s != BC_EMPTY; s = c->lines[s].hash_next) {
    if (c->lines[s].line_no == line_no) {
      return s;
    }
  }
  return BC_EMPTY;
}

static void bc_hash_insert(ntfsb_bcache *c, int32_t slot) {
  int32_t b = bc_bucket(c, c->lines[slot].line_no);
  c->lines[slot].hash_next = c->buckets[b];
  c->buckets[b] = slot;
}

static void bc_hash_remove(ntfsb_bcache *c, int32_t slot) {
  int32_t b = bc_bucket(c, c->lines[slot].line_no);
  int32_t *pp = &c->buckets[b];
  while (*pp != BC_EMPTY && *pp != slot) {
    pp = &c->lines[*pp].hash_next;
  }
  if (*pp == slot) {
    *pp = c->lines[slot].hash_next;
  }
  c->lines[slot].hash_next = BC_EMPTY;
}

static void bc_lru_unlink(ntfsb_bcache *c, int32_t slot) {
  bcache_line *l = &c->lines[slot];
  if (l->lru_prev != BC_EMPTY) {
    c->lines[l->lru_prev].lru_next = l->lru_next;
  } else if (c->lru_head == slot) {
    c->lru_head = l->lru_next;
  }
  if (l->lru_next != BC_EMPTY) {
    c->lines[l->lru_next].lru_prev = l->lru_prev;
  } else if (c->lru_tail == slot) {
    c->lru_tail = l->lru_prev;
  }
  l->lru_prev = l->lru_next = BC_EMPTY;
}

static void bc_lru_push_front(ntfsb_bcache *c, int32_t slot) {
  bcache_line *l = &c->lines[slot];
  l->lru_prev = BC_EMPTY;
  l->lru_next = c->lru_head;
  if (c->lru_head != BC_EMPTY) {
    c->lines[c->lru_head].lru_prev = slot;
  }
  c->lru_head = slot;
  if (c->lru_tail == BC_EMPTY) {
    c->lru_tail = slot;
  }
}

static void bc_touch(ntfsb_bcache *c, int32_t slot) {
  if (c->lru_head == slot) {
    return;
  }
  bc_lru_unlink(c, slot);
  bc_lru_push_front(c, slot);
}

// MARK: - 行的載入與逐出

/// 把一個髒行寫回裝置
static int bc_writeback(ntfsb_bcache *c, int32_t slot) {
  bcache_line *l = &c->lines[slot];
  if (!l->dirty) {
    return 0;
  }
  int64_t off = l->line_no * (int64_t)c->line_size;
  if (bc_direct_pwrite(c, bc_data(c, slot), c->line_size, off) != (int64_t)c->line_size) {
    return -1;
  }
  l->dirty = false;
  return 0;
}

/// 取得一個可用的槽（必要時逐出 LRU 尾端）。失敗回傳 BC_EMPTY。
static int32_t bc_take_slot(ntfsb_bcache *c) {
  int32_t slot = c->lru_tail;
  if (slot == BC_EMPTY) {
    return BC_EMPTY;
  }
  if (c->lines[slot].line_no != BC_EMPTY) {
    if (bc_writeback(c, slot) != 0) {
      return BC_EMPTY;
    }
    bc_hash_remove(c, slot);
    c->lines[slot].line_no = BC_EMPTY;
  }
  return slot;
}

/// 取得 line_no 對應的槽。fill 為 true 時 miss 會從裝置讀入整行；
/// 為 false 表示呼叫端會覆寫整行，不需要先讀。失敗回傳 BC_EMPTY。
static int32_t bc_acquire(ntfsb_bcache *c, int64_t line_no, bool fill) {
  int32_t slot = bc_find(c, line_no);
  if (slot != BC_EMPTY) {
    bc_touch(c, slot);
    return slot;
  }
  slot = bc_take_slot(c);
  if (slot == BC_EMPTY) {
    return BC_EMPTY;
  }
  if (fill) {
    int64_t off = line_no * (int64_t)c->line_size;
    if (bc_direct_pread(c, bc_data(c, slot), c->line_size, off) != (int64_t)c->line_size) {
      return BC_EMPTY;
    }
  }
  c->lines[slot].line_no = line_no;
  c->lines[slot].dirty = false;
  bc_hash_insert(c, slot);
  bc_touch(c, slot);
  return slot;
}

/// 讓 [offset, offset+count) 涵蓋的行失效。
/// flush_dirty 為 true 時先把髒行寫回（供繞過快取的寫入使用，避免遺失）。
static int bc_invalidate_range(ntfsb_bcache *c, int64_t offset, int64_t count, bool flush_dirty) {
  if (c->line_count == 0) {
    return 0;
  }
  int64_t first = offset >> c->line_shift;
  int64_t last = (offset + count - 1) >> c->line_shift;
  for (int64_t ln = first; ln <= last; ln++) {
    int32_t slot = bc_find(c, ln);
    if (slot == BC_EMPTY) {
      continue;
    }
    if (flush_dirty && bc_writeback(c, slot) != 0) {
      return -1;
    }
    bc_hash_remove(c, slot);
    c->lines[slot].line_no = BC_EMPTY;
    c->lines[slot].dirty = false;
    // 移到 LRU 尾端，優先被重用
    bc_lru_unlink(c, slot);
    c->lines[slot].lru_prev = c->lru_tail;
    c->lines[slot].lru_next = BC_EMPTY;
    if (c->lru_tail != BC_EMPTY) {
      c->lines[c->lru_tail].lru_next = slot;
    }
    c->lru_tail = slot;
    if (c->lru_head == BC_EMPTY) {
      c->lru_head = slot;
    }
  }
  return 0;
}

// MARK: - 生命週期

ntfsb_bcache *ntfsb_bcache_create(const ntfsb_device_callbacks *cb, int64_t device_size) {
  if (!cb || device_size <= 0) {
    errno = EINVAL;
    return NULL;
  }
  ntfsb_bcache *c = calloc(1, sizeof(*c));
  if (!c) {
    errno = ENOMEM;
    return NULL;
  }
  c->cb = *cb;
  c->size = device_size;
  c->line_size = NTFSB_BCACHE_LINE;
  c->line_shift = 0;
  for (uint32_t v = c->line_size; v > 1; v >>= 1) {
    c->line_shift++;
  }
  c->full_lines = device_size / c->line_size;
  c->lru_head = c->lru_tail = BC_EMPTY;

  // 行大小必須是 block_size 的整數倍，否則無法保證對齊，直接退化為轉呼叫
  if (cb->block_size == 0 || (c->line_size % cb->block_size) != 0) {
    return c;
  }

  int32_t count = NTFSB_BCACHE_BYTES / (int32_t)c->line_size;
  int32_t buckets = 1;
  while (buckets < count * 2) {
    buckets <<= 1;
  }
  c->slab = malloc((size_t)count * c->line_size);
  c->lines = malloc(sizeof(bcache_line) * (size_t)count);
  c->buckets = malloc(sizeof(int32_t) * (size_t)buckets);
  if (!c->slab || !c->lines || !c->buckets) {
    // 記憶體不足：退化為純轉呼叫，不算失敗
    free(c->slab);
    free(c->lines);
    free(c->buckets);
    c->slab = NULL;
    c->lines = NULL;
    c->buckets = NULL;
    return c;
  }
  c->line_count = count;
  c->bucket_mask = buckets - 1;
  for (int32_t i = 0; i < buckets; i++) {
    c->buckets[i] = BC_EMPTY;
  }
  for (int32_t i = 0; i < count; i++) {
    c->lines[i].line_no = BC_EMPTY;
    c->lines[i].hash_next = BC_EMPTY;
    c->lines[i].dirty = false;
    c->lines[i].lru_prev = (i == 0) ? BC_EMPTY : i - 1;
    c->lines[i].lru_next = (i == count - 1) ? BC_EMPTY : i + 1;
  }
  c->lru_head = 0;
  c->lru_tail = count - 1;
  return c;
}

void ntfsb_bcache_free(ntfsb_bcache *c) {
  if (!c) {
    return;
  }
  free(c->slab);
  free(c->lines);
  free(c->buckets);
  free(c);
}

void ntfsb_bcache_set_readonly(ntfsb_bcache *c, bool readonly) {
  if (c) {
    c->readonly = readonly;
  }
}

int ntfsb_bcache_flush(ntfsb_bcache *c) {
  if (!c) {
    errno = EINVAL;
    return -1;
  }
  for (int32_t i = 0; i < c->line_count; i++) {
    if (c->lines[i].line_no != BC_EMPTY && c->lines[i].dirty) {
      if (bc_writeback(c, i) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

// MARK: - 讀寫

int64_t ntfsb_bcache_pread(ntfsb_bcache *c, void *buf, int64_t count, int64_t offset) {
  if (!c || !buf || count < 0 || offset < 0) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  // 大請求、尾端不足一行的區域、以及退化狀態都直接走裝置
  if (c->line_count == 0 || count >= NTFSB_BCACHE_BYPASS ||
      (offset + count) > c->full_lines * (int64_t)c->line_size) {
    if (bc_invalidate_range(c, offset, count, true) != 0) {
      return -1;
    }
    return bc_direct_pread(c, buf, count, offset);
  }

  int64_t done = 0;
  while (done < count) {
    int64_t pos = offset + done;
    int64_t line_no = pos >> c->line_shift;
    int64_t in_line = pos - (line_no << c->line_shift);
    int64_t n = (int64_t)c->line_size - in_line;
    if (n > count - done) {
      n = count - done;
    }
    int32_t slot = bc_acquire(c, line_no, true);
    if (slot == BC_EMPTY) {
      // 快取失效時退回直接讀取，不讓整個請求失敗
      return done > 0 ? done : bc_direct_pread(c, buf, count, offset);
    }
    memcpy((uint8_t *)buf + done, bc_data(c, slot) + in_line, (size_t)n);
    done += n;
  }
  return done;
}

int64_t ntfsb_bcache_pwrite(ntfsb_bcache *c, const void *buf, int64_t count, int64_t offset) {
  if (!c || !buf || count < 0 || offset < 0) {
    errno = EINVAL;
    return -1;
  }
  if (c->readonly) {
    errno = EROFS;
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  if (c->line_count == 0 || count >= NTFSB_BCACHE_BYPASS ||
      (offset + count) > c->full_lines * (int64_t)c->line_size) {
    // 先把重疊的髒行寫回再直寫，之後讓它們失效，避免舊資料蓋掉新資料
    if (bc_invalidate_range(c, offset, count, true) != 0) {
      return -1;
    }
    return bc_direct_pwrite(c, buf, count, offset);
  }

  int64_t done = 0;
  while (done < count) {
    int64_t pos = offset + done;
    int64_t line_no = pos >> c->line_shift;
    int64_t in_line = pos - (line_no << c->line_shift);
    int64_t n = (int64_t)c->line_size - in_line;
    if (n > count - done) {
      n = count - done;
    }
    // 整行覆寫時不需要先從裝置讀進來
    bool whole_line = (in_line == 0 && n == (int64_t)c->line_size);
    int32_t slot = bc_acquire(c, line_no, !whole_line);
    if (slot == BC_EMPTY) {
      return done > 0 ? done : bc_direct_pwrite(c, buf, count, offset);
    }
    memcpy(bc_data(c, slot) + in_line, (const uint8_t *)buf + done, (size_t)n);
    c->lines[slot].dirty = true;
    done += n;
  }
  return done;
}
