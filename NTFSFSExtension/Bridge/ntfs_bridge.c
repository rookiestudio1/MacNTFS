//
//  ntfs_bridge.c
//  NTFSFSExtension
//
//  libntfs-3g 橋接層實作。介面說明請見 ntfs_bridge.h。
//

#include "ntfs_bridge.h"
#include "ntfs_blockcache.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

#include "ntfs-3g/types.h"
#include "ntfs-3g/layout.h"
#include "ntfs-3g/device.h"
#include "ntfs-3g/volume.h"
#include "ntfs-3g/inode.h"
#include "ntfs-3g/attrib.h"
#include "ntfs-3g/cache.h"
#include "ntfs-3g/dir.h"
#include "ntfs-3g/unistr.h"
#include "ntfs-3g/ntfstime.h"
#include "ntfs-3g/reparse.h"
#include "ntfs-3g/logging.h"
#include "ntfs-3g/misc.h"

#ifndef ENOATTR
#define ENOATTR 93
#endif

/// 對齊讀寫用的暫存緩衝區大小
#define NTFSB_BOUNCE_SIZE (256 * 1024)

/// 未知 reparse point 顯示用的假目標
#define NTFSB_BAD_REPARSE "unsupported reparse point"

// MARK: - 內部結構

/// 掛載中的卷
/// 目錄列舉快照的槽位數與閒置逾時（秒），見「目錄列舉」一節
#define NTFSB_DIRSNAP_SLOTS 16
#define NTFSB_DIRSNAP_TTL   300

typedef struct ntfsb_dirsnap_entry {
  char *name;
  size_t len;
  uint64_t mft;
  ntfsb_type type;
} ntfsb_dirsnap_entry;

typedef struct ntfsb_dirsnap {
  uint32_t id;                    ///< 0 表示空槽
  uint64_t dir_mft;
  ntfsb_dirsnap_entry *entries;
  size_t count;
  uint64_t last_tick;             ///< LRU 用
  time_t last_used;               ///< 逾時用
} ntfsb_dirsnap;

static void ntfsb_dirsnap_free(ntfsb_dirsnap *s);

struct ntfsb_volume {
  ntfs_volume *vol;
  char *label;        ///< UTF-8 卷標快取
  char *mnt_point;    ///< 解析 junction 時使用的掛載點
  ntfsb_file *open_files;  ///< 目前持有的持久 handle（見下方說明）
  ntfsb_dirsnap snaps[NTFSB_DIRSNAP_SLOTS];  ///< 進行中的目錄列舉快照
  uint32_t next_snap_id;
  uint64_t snap_tick;
};

// MARK: - 持久檔案 handle
//
// libntfs-3g 的 ntfs_inode_open 沒有引用計數：命中 nidata 快取時會把項目從快取「移除」，
// 沒命中就真的再開一份。因此同一個 mft_no 若被開啟兩次，會得到兩份獨立的 ntfs_inode，
// 兩份都可能是 dirty，最後關閉的那份會覆蓋先關的。
//
// 為了能跨多次讀寫持有 inode 與 ntfs_attr（省掉每次重開與 runlist 重建，
// 也省掉 ntfs_inode_close 觸發的 MFT 寫回），必須保證 handle 存在期間，
// 橋接層所有對該 mft_no 的操作都用同一份 inode。做法是讓 ntfsb_open_inode
// 先查 handle 串列，命中就「借用」handle 持有的 inode，對應的 ntfsb_close_inode
// 則不真的關閉。
//
// handle 只給一般檔案（ntfsb_file_open 會拒絕目錄與 reparse point），
// 所以 ntfs_dir_parent_inode 回傳的目錄 inode 不可能和 handle 衝突。
// 同時開啟的檔案通常只有個位數，串列線性搜尋即可。

/// 檔案已被刪除的 handle 改用這個 mft_no，不會與任何真實記錄相符
#define NTFSB_MFT_GONE UINT64_MAX

struct ntfsb_file {
  ntfsb_file *next;
  ntfsb_volume *v;
  uint64_t mft_no;
  ntfs_inode *ni;
  ntfs_attr *na;
  int refs;          ///< 同一個檔案被開啟多次時共用同一個 handle
  bool meta_dirty;   ///< 有尚未寫回的中繼資料（大小、時間戳）
};

/// 找出 mft_no 對應的 handle（含暫停中、目前沒有持有 inode 的）
static ntfsb_file *ntfsb_find_file_any(ntfsb_volume *v, uint64_t mft_no) {
  for (ntfsb_file *f = v->open_files; f; f = f->next) {
    if (f->mft_no == mft_no) {
      return f;
    }
  }
  return NULL;
}

/// 找出 mft_no 對應且仍持有 inode 的 handle，沒有則回傳 NULL。
/// 暫停中的 handle 不會被借出，相關路徑會自動退回「每次重開」的行為。
static ntfsb_file *ntfsb_find_file(ntfsb_volume *v, uint64_t mft_no) {
  ntfsb_file *f = ntfsb_find_file_any(v, mft_no);
  return (f && f->ni) ? f : NULL;
}

/// 這個 inode 是否由某個 handle 持有
static bool ntfsb_inode_is_held(ntfsb_volume *v, const ntfs_inode *ni) {
  for (ntfsb_file *f = v->open_files; f; f = f->next) {
    if (f->ni == ni) {
      return true;
    }
  }
  return false;
}

/// 裝置私有資料
typedef struct ntfsb_dev_priv {
  ntfsb_device_callbacks cb;
  int64_t pos;            ///< 目前檔案位置（seek/read/write 用）
  uint8_t *bounce;        ///< 對齊用暫存緩衝區
  int64_t size;           ///< 裝置總位元組數
  ntfsb_bcache *cache;    ///< 區塊快取（見 ntfs_blockcache.h）
} ntfsb_dev_priv;

/// 釋放裝置私有資料。ntfs_umount 會自行釋放 dev 本體，所以有幾處要單獨呼叫這個。
static void ntfsb_dev_priv_destroy(ntfsb_dev_priv *p) {
  if (!p) {
    return;
  }
  ntfsb_bcache_free(p->cache);
  free(p->bounce);
  free(p);
}

// MARK: - 小工具

/// 把 errno 轉成負值回傳，errno 為 0 時回傳 -EIO
static int ntfsb_neg_errno(void) {
  int e = errno;
  return e ? -e : -EIO;
}

/// 將 UTF-8 名稱轉成 NTFS Unicode。成功回傳長度（字元數），失敗回傳負 errno。
static int ntfsb_name_to_ucs(const char *name, ntfschar **out) {
  int len = ntfs_mbstoucs(name, out);
  if (len < 0) {
    return ntfsb_neg_errno();
  }
  if (len == 0 || len > NTFS_MAX_NAME_LEN) {
    free(*out);
    *out = NULL;
    return -ENAMETOOLONG;
  }
  return len;
}

/// 把名稱轉成 NFC，給 libntfs-3g 的 lookup 快取當鍵。
/// 快取直接比對名稱位元組（連「找不到」也會記住），同一名稱的 NFC／NFD 兩種寫法
/// 會變成兩筆互不相干的記錄：先用 NFC 查過不存在的名稱，Finder 再以 NFD 建立它，
/// NFC 那筆負向記錄就不會被更新，之後一直查不到。
/// 實際查詢時 ntfs_mbstoucs 本來就會轉成 NFC，所以統一鍵值不影響查詢結果。
/// 回傳新配置的字串（呼叫端 free）；純 ASCII 或無法正規化時回傳 NULL，呼叫端沿用原名稱。
static char *ntfsb_name_nfc(const char *name) {
#ifdef ENABLE_NFCONV
  const unsigned char *p = (const unsigned char *)name;
  while (*p && *p < 0x80) {
    p++;
  }
  if (!*p) {
    return NULL;
  }
  char *nfc = NULL;
  if (ntfs_macosx_normalize_utf8(name, &nfc, 1) >= 0) {
    return nfc;
  }
#else
  (void)name;
#endif
  return NULL;
}

/// ntfs_inode_lookup_by_mbsname 的包裝：先統一成 NFC，見 ntfsb_name_nfc
static u64 ntfsb_lookup_by_mbsname(ntfs_inode *dir_ni, const char *name) {
  char *nfc = ntfsb_name_nfc(name);
  u64 inum = ntfs_inode_lookup_by_mbsname(dir_ni, nfc ? nfc : name);
  int err = errno;
  free(nfc);
  errno = err;
  return inum;
}

/// ntfs_inode_update_mbsname 的包裝：先統一成 NFC，見 ntfsb_name_nfc
static void ntfsb_update_mbsname(ntfs_inode *dir_ni, const char *name, u64 inum) {
  char *nfc = ntfsb_name_nfc(name);
  ntfs_inode_update_mbsname(dir_ni, nfc ? nfc : name, inum);
  free(nfc);
}

/// 檢查名稱是否包含 NTFS 不允許的字元（保持 Windows 相容）
static bool ntfsb_name_is_forbidden(const ntfschar *uname, int len) {
  return ntfs_forbidden_chars(uname, len, TRUE);
}

/// 依 ntfs_inode 判斷檔案型別（不會開啟額外屬性）
static ntfsb_type ntfsb_type_of_inode(ntfs_inode *ni, ntfs_attr **out_na) {
  if (out_na) {
    *out_na = NULL;
  }
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    return NTFSB_TYPE_DIR;
  }
  if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    return NTFSB_TYPE_SYMLINK;
  }
  if (ni->flags & FILE_ATTR_SYSTEM) {
    // Interix 特殊檔案（FIFO / socket / symlink / 裝置）以 SYSTEM 屬性 + $DATA 內容標示
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      return NTFSB_TYPE_FILE;
    }
    ntfsb_type type = NTFSB_TYPE_FILE;
    if (!(ni->flags & FILE_ATTR_HIDDEN)) {
      if (na->data_size == 0) {
        type = NTFSB_TYPE_FIFO;
      } else if (na->data_size == 1) {
        type = NTFSB_TYPE_SOCK;
      }
    }
    if (type == NTFSB_TYPE_FILE &&
        (u64)na->data_size > sizeof(INTX_FILE_TYPES) &&
        (u64)na->data_size <= sizeof(INTX_FILE_TYPES) + sizeof(ntfschar) * PATH_MAX) {
      INTX_FILE *intx = ntfs_malloc(na->data_size);
      if (intx && ntfs_attr_pread(na, 0, na->data_size, intx) == na->data_size) {
        if (intx->magic == INTX_SYMBOLIC_LINK) {
          type = NTFSB_TYPE_SYMLINK;
        } else if (intx->magic == INTX_BLOCK_DEVICE &&
                   na->data_size == offsetof(INTX_FILE, device_end)) {
          type = NTFSB_TYPE_BLK;
        } else if (intx->magic == INTX_CHARACTER_DEVICE &&
                   na->data_size == offsetof(INTX_FILE, device_end)) {
          type = NTFSB_TYPE_CHR;
        }
      }
      free(intx);
    }
    if (out_na) {
      *out_na = na;
    } else {
      ntfs_attr_close(na);
    }
    return type;
  }
  return NTFSB_TYPE_FILE;
}

/// 開啟 inode，失敗回傳 NULL 並設定 errno。
/// 若該檔案目前有持久 handle，直接借用 handle 持有的 inode（不可自行關閉）。
static ntfs_inode *ntfsb_open_inode(ntfsb_volume *v, uint64_t mft_no) {
  ntfsb_file *f = ntfsb_find_file(v, mft_no);
  if (f) {
    return f->ni;
  }
  return ntfs_inode_open(v->vol, (MFT_REF)mft_no);
}

/// 與 ntfsb_open_inode 配對的關閉。借用自 handle 的 inode 不會真的關閉。
/// 對於不是經由 ntfsb_open_inode 取得的 inode（例如 ntfs_dir_parent_inode 的回傳值）
/// 一樣安全：找不到持有者就走一般關閉。
static int ntfsb_close_inode(ntfsb_volume *v, ntfs_inode *ni) {
  if (!ni) {
    return 0;
  }
  if (ntfsb_inode_is_held(v, ni)) {
    return 0;
  }
  return ntfs_inode_close(ni);
}

// MARK: - 裝置操作（ntfs_device_operations）

static ntfsb_dev_priv *ntfsb_dev(struct ntfs_device *dev) {
  return (ntfsb_dev_priv *)dev->d_private;
}

static int ntfsb_dev_open(struct ntfs_device *dev, int flags) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  if (NDevOpen(dev)) {
    errno = EBUSY;
    return -1;
  }
  bool want_write = (flags & O_ACCMODE) != O_RDONLY;
  if (want_write && !p->cb.writable) {
    // 讓 libntfs-3g 得以退回唯讀
    errno = EROFS;
    return -1;
  }
  if (!p->bounce) {
    p->bounce = malloc(NTFSB_BOUNCE_SIZE);
    if (!p->bounce) {
      errno = ENOMEM;
      return -1;
    }
  }
  p->pos = 0;
  ntfsb_bcache_set_readonly(p->cache, !want_write);
  if (!want_write) {
    NDevSetReadOnly(dev);
  } else {
    NDevClearReadOnly(dev);
  }
  NDevSetBlock(dev);
  NDevSetOpen(dev);
  return 0;
}

static int ntfsb_dev_close(struct ntfs_device *dev) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  if (!NDevOpen(dev)) {
    errno = EBADF;
    return -1;
  }
  // 區塊快取是 write-back，關閉前一定要把髒行寫回
  if (!NDevReadOnly(dev)) {
    ntfsb_bcache_flush(p->cache);
  }
  if (NDevDirty(dev) && p->cb.sync) {
    p->cb.sync(p->cb.context);
  }
  NDevClearOpen(dev);
  return 0;
}

static s64 ntfsb_dev_seek(struct ntfs_device *dev, s64 offset, int whence) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  s64 base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = p->pos; break;
    case SEEK_END: base = p->size; break;
    default:
      errno = EINVAL;
      return -1;
  }
  s64 np = base + offset;
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  p->pos = np;
  return np;
}

/// 對齊的區塊讀取：offset 與 count 必須是 block_size 倍數
static s64 ntfsb_dev_pread_aligned(ntfsb_dev_priv *p, void *buf, s64 count, s64 offset) {
  return ntfsb_bcache_pread(p->cache, buf, count, offset);
}

static s64 ntfsb_dev_pwrite_aligned(ntfsb_dev_priv *p, const void *buf, s64 count, s64 offset) {
  return ntfsb_bcache_pwrite(p->cache, buf, count, offset);
}

static s64 ntfsb_dev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  const s64 bs = p->cb.block_size;
  if (count < 0 || offset < 0) {
    errno = EINVAL;
    return -1;
  }
  if (offset >= p->size) {
    return 0;
  }
  if (offset + count > p->size) {
    count = p->size - offset;
  }
  if (count == 0) {
    return 0;
  }
  // 已對齊：直接讀
  if ((offset % bs) == 0 && (count % bs) == 0) {
    return ntfsb_dev_pread_aligned(p, buf, count, offset);
  }
  // 未對齊：分段透過暫存緩衝區讀取再複製
  s64 done = 0;
  while (done < count) {
    s64 cur = offset + done;
    s64 aligned = cur - (cur % bs);
    s64 head = cur - aligned;
    // 只讀「涵蓋剩餘請求」所需的對齊長度。先前這裡無條件讀滿整個 bounce buffer，
    // 使得每個未對齊的小讀取（例如查詢 $MFT 的 $BITMAP）都要搬 256 KB。
    // p->size 是 block_count * block_size，所以三個來源都是 bs 的整數倍。
    s64 need = head + (count - done);
    s64 chunk = ((need + bs - 1) / bs) * bs;
    if (chunk > NTFSB_BOUNCE_SIZE) {
      chunk = NTFSB_BOUNCE_SIZE;
    }
    if (aligned + chunk > p->size) {
      chunk = p->size - aligned;
    }
    s64 got = ntfsb_dev_pread_aligned(p, p->bounce, chunk, aligned);
    if (got < 0) {
      return -1;
    }
    if (got <= head) {
      break;
    }
    s64 avail = got - head;
    s64 want = count - done;
    s64 n = avail < want ? avail : want;
    memcpy((uint8_t *)buf + done, p->bounce + head, (size_t)n);
    done += n;
    if (got < chunk) {
      break;
    }
  }
  return done;
}

static s64 ntfsb_dev_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  const s64 bs = p->cb.block_size;
  if (NDevReadOnly(dev)) {
    errno = EROFS;
    return -1;
  }
  if (count < 0 || offset < 0 || offset + count > p->size) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  NDevSetDirty(dev);
  if ((offset % bs) == 0 && (count % bs) == 0) {
    return ntfsb_dev_pwrite_aligned(p, buf, count, offset);
  }
  // 未對齊：頭尾不完整磁區採讀-改-寫，中間對齊區段直接寫
  s64 done = 0;
  // 頭段
  if (offset % bs) {
    s64 aligned = offset - (offset % bs);
    s64 head = offset - aligned;
    s64 n = bs - head;
    if (n > count) {
      n = count;
    }
    if (ntfsb_dev_pread_aligned(p, p->bounce, bs, aligned) != bs) {
      errno = EIO;
      return -1;
    }
    memcpy(p->bounce + head, buf, (size_t)n);
    if (ntfsb_dev_pwrite_aligned(p, p->bounce, bs, aligned) != bs) {
      return -1;
    }
    done += n;
  }
  // 中段
  s64 mid = (count - done) - ((count - done) % bs);
  if (mid > 0) {
    if (ntfsb_dev_pwrite_aligned(p, (const uint8_t *)buf + done, mid, offset + done) != mid) {
      return -1;
    }
    done += mid;
  }
  // 尾段
  if (done < count) {
    s64 aligned = offset + done;
    s64 n = count - done;
    if (ntfsb_dev_pread_aligned(p, p->bounce, bs, aligned) != bs) {
      errno = EIO;
      return -1;
    }
    memcpy(p->bounce, (const uint8_t *)buf + done, (size_t)n);
    if (ntfsb_dev_pwrite_aligned(p, p->bounce, bs, aligned) != bs) {
      return -1;
    }
    done += n;
  }
  return done;
}

static s64 ntfsb_dev_read(struct ntfs_device *dev, void *buf, s64 count) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  s64 r = ntfsb_dev_pread(dev, buf, count, p->pos);
  if (r > 0) {
    p->pos += r;
  }
  return r;
}

static s64 ntfsb_dev_write(struct ntfs_device *dev, const void *buf, s64 count) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  s64 r = ntfsb_dev_pwrite(dev, buf, count, p->pos);
  if (r > 0) {
    p->pos += r;
  }
  return r;
}

static int ntfsb_dev_sync(struct ntfs_device *dev) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  int res = 0;
  if (!NDevReadOnly(dev)) {
    // 區塊快取的髒行必須先落盤，才輪到裝置本身的 sync
    if (ntfsb_bcache_flush(p->cache) != 0) {
      return -1;
    }
    if (p->cb.sync) {
      res = p->cb.sync(p->cb.context);
    }
  }
  if (res == 0) {
    NDevClearDirty(dev);
  }
  return res;
}

static int ntfsb_dev_stat(struct ntfs_device *dev, struct stat *buf) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  memset(buf, 0, sizeof(*buf));
  buf->st_mode = S_IFBLK | 0600;
  buf->st_size = p->size;
  buf->st_blksize = (blksize_t)p->cb.block_size;
  buf->st_blocks = p->size / 512;
  return 0;
}

static int ntfsb_dev_ioctl(struct ntfs_device *dev, unsigned long request, void *argp) {
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  switch (request) {
    case DKIOCGETBLOCKSIZE:
    case DKIOCGETPHYSICALBLOCKSIZE:
      *(uint32_t *)argp = p->cb.block_size;
      return 0;
    case DKIOCGETBLOCKCOUNT:
      *(uint64_t *)argp = p->cb.block_count;
      return 0;
    default:
      errno = ENOTTY;
      return -1;
  }
}

static struct ntfs_device_operations ntfsb_device_ops = {
  .open = ntfsb_dev_open,
  .close = ntfsb_dev_close,
  .seek = ntfsb_dev_seek,
  .read = ntfsb_dev_read,
  .write = ntfsb_dev_write,
  .pread = ntfsb_dev_pread,
  .pwrite = ntfsb_dev_pwrite,
  .sync = ntfsb_dev_sync,
  .stat = ntfsb_dev_stat,
  .ioctl = ntfsb_dev_ioctl,
};

/// 建立 libntfs-3g 裝置物件
static struct ntfs_device *ntfsb_device_create(const char *name, const ntfsb_device_callbacks *cb) {
  if (!cb || !cb->pread || cb->block_size == 0 || (cb->block_size & (cb->block_size - 1))) {
    errno = EINVAL;
    return NULL;
  }
  ntfsb_dev_priv *p = calloc(1, sizeof(*p));
  if (!p) {
    errno = ENOMEM;
    return NULL;
  }
  p->cb = *cb;
  p->size = (int64_t)cb->block_count * (int64_t)cb->block_size;
  p->cache = ntfsb_bcache_create(cb, p->size);
  if (!p->cache) {
    free(p);
    errno = ENOMEM;
    return NULL;
  }
  struct ntfs_device *dev = ntfs_device_alloc(name ? name : "fskit", 0, &ntfsb_device_ops, p);
  if (!dev) {
    ntfsb_dev_priv_destroy(p);
    errno = ENOMEM;
    return NULL;
  }
  return dev;
}

/// 釋放裝置物件（ntfs_umount 會呼叫 ntfs_device_free，但不會釋放我們的私有資料）
static void ntfsb_device_priv_free(struct ntfs_device *dev) {
  if (!dev) {
    return;
  }
  ntfsb_dev_priv *p = ntfsb_dev(dev);
  if (p) {
    ntfsb_dev_priv_destroy(p);
    dev->d_private = NULL;
  }
}

// MARK: - 初始化

void ntfsb_global_init(void) {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  ntfs_set_locale();
  ntfs_set_char_encoding("UTF-8");
#ifdef ENABLE_NFCONV
  // macOS 慣例：檔名讀出時轉成 NFD，寫入時轉成 NFC
  ntfs_macosx_normalize_filenames(1);
#endif
  ntfs_log_set_handler(ntfs_log_handler_stderr);
  ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_WARNING | NTFS_LOG_LEVEL_CRITICAL |
                      NTFS_LOG_LEVEL_PERROR);
}

static ntfsb_log_sink g_log_sink = NULL;

/// libntfs-3g 的 handler：把訊息格式化成單行後交給接收器
static int ntfsb_log_handler(const char *function, const char *file, int line, u32 level,
                             void *data, const char *format, va_list args) {
  (void)file;
  (void)line;
  (void)data;
  char body[512];
  int n = vsnprintf(body, sizeof(body), format, args);
  if (n < 0) {
    return 0;
  }
  // libntfs 的訊息通常以換行結尾，去掉以免 os_log 多一行
  size_t len = strnlen(body, sizeof(body));
  while (len > 0 && body[len - 1] == '\n') {
    body[--len] = '\0';
  }
  char msg[640];
  if (level & NTFS_LOG_LEVEL_PERROR) {
    snprintf(msg, sizeof(msg), "%s: %s: %s", function ? function : "?", body, strerror(errno));
  } else {
    snprintf(msg, sizeof(msg), "%s: %s", function ? function : "?", body);
  }
  bool is_error = (level & (NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_PERROR)) != 0;
  g_log_sink(is_error, msg);
  return n;
}

void ntfsb_set_log_sink(ntfsb_log_sink sink) {
  g_log_sink = sink;
  ntfs_log_set_handler(sink ? ntfsb_log_handler : ntfs_log_handler_stderr);
}

// MARK: - probe / mount / umount

/// 內部：以指定旗標掛載，成功回傳 ntfs_volume（裝置擁有權交給 vol）
static ntfs_volume *ntfsb_mount_internal(const char *name,
                                         const ntfsb_device_callbacks *cb,
                                         ntfs_mount_flags flags,
                                         int *out_errno) {
  struct ntfs_device *dev = ntfsb_device_create(name, cb);
  if (!dev) {
    if (out_errno) {
      *out_errno = errno;
    }
    return NULL;
  }
  ntfs_volume *vol = ntfs_device_mount(dev, flags);
  if (!vol) {
    int e = errno;
    ntfsb_device_priv_free(dev);
    ntfs_device_free(dev);
    if (out_errno) {
      *out_errno = e ? e : EIO;
    }
    return NULL;
  }
  // libntfs-3g 只在 ntfs_mount() 裡建立 LRU 快取，走 ntfs_device_mount() 不會建立，
  // 於是 inode / nidata / lookup / securid 快取全是 NULL，每次 ntfs_inode_open
  // 都得重新從裝置讀一次 MFT record。這裡補上；釋放由 ntfs_umount 內的
  // __ntfs_volume_release 自行處理。
  ntfs_create_lru_caches(vol);
  return vol;
}

int ntfsb_probe(const ntfsb_device_callbacks *cb, ntfsb_probe_info *info) {
  if (!cb || !info || cb->block_size < 512) {
    return -EINVAL;
  }
  memset(info, 0, sizeof(*info));
  // 讀取 boot sector
  uint8_t *sector = calloc(1, cb->block_size);
  if (!sector) {
    return -ENOMEM;
  }
  int64_t got = cb->pread(cb->context, sector, cb->block_size, 0);
  if (got < 512) {
    free(sector);
    return -EIO;
  }
  if (memcmp(sector + 3, "NTFS    ", 8) != 0) {
    free(sector);
    return -EINVAL;
  }
  uint64_t serial;
  memcpy(&serial, sector + 0x48, sizeof(serial));
  info->serial = serial;
  free(sector);

  // 唯讀掛載取得卷標與狀態（不做任何寫入）
  int err = 0;
  ntfsb_device_callbacks ro_cb = *cb;
  ro_cb.writable = false;
  ntfs_volume *vol = ntfsb_mount_internal("probe", &ro_cb,
                                          NTFS_MNT_RDONLY | NTFS_MNT_IGNORE_HIBERFILE, &err);
  if (!vol) {
    // 能辨識為 NTFS，但無法解析（損毀或版本不支援）
    return err == EINVAL ? -EIO : -err;
  }
  info->major = vol->major_ver;
  info->minor = vol->minor_ver;
  info->dirty = (vol->flags & VOLUME_IS_DIRTY) != 0;
  if (vol->vol_name) {
    strlcpy(info->label, vol->vol_name, sizeof(info->label));
  }
  errno = 0;
  if (ntfs_volume_check_hiberfile(vol, 0) < 0 && errno == EPERM) {
    info->hibernated = true;
  }
  // ntfs_umount 會釋放 dev 本體，我們的私有資料要自己釋放
  ntfsb_dev_priv *priv = vol->dev ? ntfsb_dev(vol->dev) : NULL;
  ntfs_umount(vol, TRUE);
  ntfsb_dev_priv_destroy(priv);
  return 0;
}

ntfsb_volume *ntfsb_mount(const char *name,
                          const ntfsb_device_callbacks *cb,
                          bool read_only,
                          int *out_errno) {
  if (out_errno) {
    *out_errno = 0;
  }
  ntfsb_volume *v = calloc(1, sizeof(*v));
  if (!v) {
    if (out_errno) {
      *out_errno = ENOMEM;
    }
    return NULL;
  }
  ntfs_mount_flags flags = 0;
  if (read_only || !cb->writable) {
    flags |= NTFS_MNT_RDONLY;
  } else {
    // 允許在休眠或裝置唯讀時自動退回唯讀；未正常卸載時重設日誌
    flags |= NTFS_MNT_MAY_RDONLY | NTFS_MNT_RECOVER;
  }
  int err = 0;
  ntfs_volume *vol = ntfsb_mount_internal(name, cb, flags, &err);
  if (!vol) {
    free(v);
    if (out_errno) {
      *out_errno = err;
    }
    return NULL;
  }
  v->vol = vol;
  v->mnt_point = strdup("/");
  // 不區分大小寫但保留大小寫：清除大小寫敏感旗標讓查找走 IGNORE_CASE collation，
  // 但 locase 表改用「恆等」表，避免 ntfs_readdir 把檔名全部轉成小寫。
  if (vol->upcase && vol->upcase_len) {
    ntfschar *identity = malloc(vol->upcase_len * sizeof(ntfschar));
    if (identity) {
      for (u32 i = 0; i < vol->upcase_len; i++) {
        identity[i] = cpu_to_le16((u16)i);
      }
      free(vol->locase);
      vol->locase = identity;
      NVolClearCaseSensitive(vol);
    }
  }
  // 不顯示 $MFT 等系統中繼檔；顯示隱藏檔（由上層以 UF_HIDDEN 標示）；不自動隱藏點檔
  ntfs_set_shown_files(vol, FALSE, TRUE, FALSE);
  // 掃描 $Bitmap 以便 statfs 快速回報可用空間
  if (ntfs_volume_get_free_space(vol)) {
    ntfs_log_perror("無法讀取 NTFS $Bitmap");
  }
  v->label = strdup(vol->vol_name ? vol->vol_name : "");
  return v;
}

int ntfsb_umount(ntfsb_volume *v) {
  if (!v) {
    return -EINVAL;
  }
  int res = 0;
  // 卸載前先關掉所有殘留的 handle，把延後的中繼資料寫回
  while (v->open_files) {
    v->open_files->refs = 1;
    ntfsb_file_close(v->open_files);
  }
  if (v->vol) {
    struct ntfs_device *dev = v->vol->dev;
    ntfsb_dev_priv *priv = dev ? ntfsb_dev(dev) : NULL;
    if (ntfs_umount(v->vol, FALSE)) {
      res = ntfsb_neg_errno();
    }
    // ntfs_umount 已釋放 dev 本體
    ntfsb_dev_priv_destroy(priv);
    v->vol = NULL;
  }
  for (int i = 0; i < NTFSB_DIRSNAP_SLOTS; i++) {
    ntfsb_dirsnap_free(&v->snaps[i]);
  }
  free(v->label);
  free(v->mnt_point);
  free(v);
  return res;
}

bool ntfsb_is_readonly(const ntfsb_volume *v) {
  return !v || !v->vol || NVolReadOnly(v->vol);
}

const char *ntfsb_label(const ntfsb_volume *v) {
  return (v && v->label) ? v->label : "";
}

int ntfsb_set_label(ntfsb_volume *v, const char *label) {
  if (!v || !v->vol || !label) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfschar *ulabel = NULL;
  int len = ntfs_mbstoucs(label, &ulabel);
  if (len < 0) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (ntfs_volume_rename(v->vol, ulabel, len)) {
    res = ntfsb_neg_errno();
  } else {
    free(v->label);
    v->label = strdup(label);
  }
  free(ulabel);
  return res;
}

int ntfsb_statfs(ntfsb_volume *v, ntfsb_statfs_info *out) {
  if (!v || !v->vol || !out) {
    return -EINVAL;
  }
  ntfs_volume *vol = v->vol;
  memset(out, 0, sizeof(*out));
  out->cluster_size = vol->cluster_size;
  out->sector_size = vol->sector_size;
  out->total_clusters = (uint64_t)vol->nr_clusters;
  s64 free_clusters = vol->free_clusters;
  if (free_clusters < 0) {
    free_clusters = 0;
  }
  out->free_clusters = (uint64_t)free_clusters;
  // 可用空間可再容納的 MFT 記錄數
  s64 size = free_clusters;
  int delta_bits = vol->cluster_size_bits - vol->mft_record_size_bits;
  if (delta_bits >= 0) {
    size <<= delta_bits;
  } else {
    size >>= -delta_bits;
  }
  out->total_files = (uint64_t)((vol->mftbmp_na->allocated_size << 3) + size);
  size += vol->free_mft_records;
  if (size < 0) {
    size = 0;
  }
  out->free_files = (uint64_t)size;
  return 0;
}

int ntfsb_sync(ntfsb_volume *v) {
  if (!v || !v->vol) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return 0;
  }
  // 持久 handle 上延後的中繼資料要先寫回，才輪到 $MFT / $Bitmap
  ntfsb_flush_all(v);
  // 將 $MFT / $Bitmap 等內部 inode 的變更寫回，再同步裝置
  if (v->vol->mft_ni) {
    ntfs_inode_sync(v->vol->mft_ni);
  }
  if (v->vol->lcnbmp_ni) {
    ntfs_inode_sync(v->vol->lcnbmp_ni);
  }
  if (v->vol->dev && ntfs_device_sync(v->vol->dev)) {
    return ntfsb_neg_errno();
  }
  return 0;
}

// MARK: - 屬性與查找

int ntfsb_getattr(ntfsb_volume *v, uint64_t mft_no, ntfsb_attr *out) {
  if (!v || !v->vol || !out) {
    return -EINVAL;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  memset(out, 0, sizeof(*out));
  out->mft_no = ni->mft_no;
  out->nlink = le16_to_cpu(ni->mrec->link_count);
  ntfs_attr *na = NULL;
  out->type = ntfsb_type_of_inode(ni, &na);
  if (out->type == NTFSB_TYPE_DIR) {
    // 目錄大小：以 $INDEX_ALLOCATION 的大小為準
    if (!test_nino_flag(ni, KnownSize)) {
      ntfs_attr *ina = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
      if (ina) {
        ni->data_size = ina->data_size;
        ni->allocated_size = ina->allocated_size;
        set_nino_flag(ni, KnownSize);
        ntfs_attr_close(ina);
      }
    }
    out->size = (uint64_t)ni->data_size;
    out->alloc_size = (uint64_t)ni->allocated_size;
    out->nlink = 1;
  } else if (out->type == NTFSB_TYPE_SYMLINK && (ni->flags & FILE_ATTR_REPARSE_POINT)) {
    // reparse point：大小為解析後的目標字串長度
    errno = 0;
    char *target = ntfs_make_symlink(ni, v->mnt_point);
    if (target) {
      out->size = strlen(target);
      free(target);
    } else {
      out->size = strlen(NTFSB_BAD_REPARSE);
    }
    out->alloc_size = (uint64_t)ni->allocated_size;
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
        bool borrowed = (held && da == held->na) || (da == na);
        if (!borrowed) {
          ntfs_attr_close(da);
        }
      } else {
        // 開不了 $DATA 的檔案不交給 kernel
        out->resident = true;
      }
    }
  }
  if (na) {
    ntfs_attr_close(na);
  }
  out->readonly = (ni->flags & FILE_ATTR_READONLY) != 0;
  out->hidden = (ni->flags & FILE_ATTR_HIDDEN) != 0;
  out->system = (ni->flags & FILE_ATTR_SYSTEM) != 0;
  out->archive = (ni->flags & FILE_ATTR_ARCHIVE) != 0;
  out->compressed = (ni->flags & FILE_ATTR_COMPRESSED) != 0;
  out->encrypted = (ni->flags & FILE_ATTR_ENCRYPTED) != 0;
  out->sparse = (ni->flags & FILE_ATTR_SPARSE_FILE) != 0;
  out->atime = ntfs2timespec(ni->last_access_time);
  out->mtime = ntfs2timespec(ni->last_data_change_time);
  out->ctime = ntfs2timespec(ni->last_mft_change_time);
  out->btime = ntfs2timespec(ni->creation_time);
  int res = 0;
  if (ntfsb_close_inode(v, ni)) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_lookup(ntfsb_volume *v,
                 uint64_t dir_mft_no,
                 const char *name,
                 uint64_t *out_mft_no,
                 ntfsb_type *out_type) {
  if (!v || !v->vol || !name || !out_mft_no) {
    return -EINVAL;
  }
  if (!strcmp(name, ".")) {
    *out_mft_no = dir_mft_no;
    if (out_type) {
      *out_type = NTFSB_TYPE_DIR;
    }
    return 0;
  }
  ntfs_inode *dir_ni = ntfsb_open_inode(v, dir_mft_no);
  if (!dir_ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  u64 inum;
  if (!strcmp(name, "..")) {
    ntfs_inode *parent = ntfs_dir_parent_inode(dir_ni);
    if (!parent) {
      res = ntfsb_neg_errno();
      ntfsb_close_inode(v, dir_ni);
      return res;
    }
    inum = parent->mft_no;
    ntfsb_close_inode(v, parent);
  } else {
    inum = ntfsb_lookup_by_mbsname(dir_ni, name);
    if (inum == (u64)-1) {
      res = ntfsb_neg_errno();
      ntfsb_close_inode(v, dir_ni);
      return res;
    }
    inum = MREF(inum);
  }
  ntfsb_close_inode(v, dir_ni);
  *out_mft_no = inum;
  if (out_type) {
    ntfs_inode *ni = ntfsb_open_inode(v, inum);
    if (!ni) {
      return ntfsb_neg_errno();
    }
    *out_type = ntfsb_type_of_inode(ni, NULL);
    ntfsb_close_inode(v, ni);
  }
  return 0;
}

int ntfsb_parent(ntfsb_volume *v, uint64_t mft_no, uint64_t *out_parent) {
  if (!v || !v->vol || !out_parent) {
    return -EINVAL;
  }
  if (mft_no == NTFSB_ROOT_MFT_NO) {
    *out_parent = NTFSB_ROOT_MFT_NO;
    return 0;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  ntfs_inode *parent = ntfs_dir_parent_inode(ni);
  if (!parent) {
    res = ntfsb_neg_errno();
  } else {
    *out_parent = parent->mft_no;
    ntfsb_close_inode(v, parent);
  }
  ntfsb_close_inode(v, ni);
  return res;
}

// MARK: - 目錄列舉

/// 列舉過程中的暫存狀態
typedef struct ntfsb_readdir_ctx {
  ntfsb_readdir_cb cb;
  void *context;
  bool has_pending;
  char *pending_name;
  size_t pending_len;
  uint64_t pending_mft;
  ntfsb_type pending_type;
  int64_t pending_cookie;
  int64_t resume_pos;   ///< 停止時應回傳給呼叫端的 cookie
  bool stopped;
  int error;
} ntfsb_readdir_ctx;

static ntfsb_type ntfsb_type_from_dt(unsigned dt_type) {
  switch (dt_type) {
    case NTFS_DT_DIR: return NTFSB_TYPE_DIR;
    case NTFS_DT_LNK:
    case NTFS_DT_REPARSE: return NTFSB_TYPE_SYMLINK;
    case NTFS_DT_FIFO: return NTFSB_TYPE_FIFO;
    case NTFS_DT_SOCK: return NTFSB_TYPE_SOCK;
    case NTFS_DT_BLK: return NTFSB_TYPE_BLK;
    case NTFS_DT_CHR: return NTFSB_TYPE_CHR;
    default: return NTFSB_TYPE_FILE;
  }
}

/// 把暫存的項目交給呼叫端；next 為下一個項目的 cookie
static int ntfsb_readdir_flush(ntfsb_readdir_ctx *c, int64_t next) {
  if (!c->has_pending) {
    return 0;
  }
  int stop = c->cb(c->context, c->pending_name, c->pending_len, c->pending_mft,
                   c->pending_type, c->pending_cookie, next);
  if (stop) {
    // 呼叫端拒收：下次應從這個項目重新開始
    c->resume_pos = c->pending_cookie;
    c->stopped = true;
    return 1;
  }
  c->resume_pos = next;
  free(c->pending_name);
  c->pending_name = NULL;
  c->has_pending = false;
  return 0;
}

static int ntfsb_filldir(void *dirent, const ntfschar *name, const int name_len,
                         const int name_type, const s64 pos, const MFT_REF mref,
                         const unsigned dt_type) {
  ntfsb_readdir_ctx *c = dirent;
  if (name_type == FILE_NAME_DOS) {
    // 略過 8.3 短檔名
    return 0;
  }
  // 先把前一個項目交出去（此時才知道它的 next cookie）
  if (ntfsb_readdir_flush(c, pos)) {
    return 1;
  }
  char *utf8 = NULL;
  int len = ntfs_ucstombs(name, name_len, &utf8, 0);
  if (len < 0) {
    ntfs_log_perror("檔名解碼失敗（inode %llu）", (unsigned long long)MREF(mref));
    return 0;
  }
  if (len > NTFS_MAX_NAME_LEN) {
    // macOS 檔名上限 255 位元組，超過會被略過
    free(utf8);
    return 0;
  }
  c->pending_name = utf8;
  c->pending_len = (size_t)len;
  c->pending_mft = MREF(mref);
  c->pending_type = ntfsb_type_from_dt(dt_type);
  c->pending_cookie = pos;
  c->has_pending = true;
  return 0;
}

/// 以 NTFS 索引位置為 cookie 的原始列舉。位置在目錄有刪除時會往前挪，
/// 只用來建立快照，不直接交給 FSKit。
static int ntfsb_readdir_index(ntfsb_volume *v,
                               uint64_t dir_mft_no,
                               int64_t *pos,
                               ntfsb_readdir_cb callback,
                               void *context) {
  if (*pos >= NTFSB_COOKIE_END) {
    return 0;
  }
  ntfs_inode *dir_ni = ntfsb_open_inode(v, dir_mft_no);
  if (!dir_ni) {
    return ntfsb_neg_errno();
  }
  if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
    ntfsb_close_inode(v, dir_ni);
    return -ENOTDIR;
  }
  ntfsb_readdir_ctx c = {
    .cb = callback,
    .context = context,
    .resume_pos = NTFSB_COOKIE_END,
  };
  s64 p = (s64)*pos;
  errno = 0;
  int rc = ntfs_readdir(dir_ni, &p, &c, ntfsb_filldir);
  int res = 0;
  if (rc && !c.stopped) {
    res = ntfsb_neg_errno();
  } else if (!c.stopped) {
    // 正常結束：交出最後一個項目
    if (ntfsb_readdir_flush(&c, NTFSB_COOKIE_END)) {
      // 呼叫端拒收，resume_pos 已指向該項目
    }
  }
  free(c.pending_name);
  *pos = c.resume_pos;
  if (ntfsb_close_inode(v, dir_ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

// MARK: - 目錄列舉快照
//
// NTFS 目錄是 B+ tree 索引，刪除項目後同一個區塊裡後面的項目會往前挪，
// 因此 ntfs_readdir 的位置不能當成穩定的 cookie：Finder 清空垃圾桶時邊列邊刪，
// 續傳會跳過項目，最後 rmdir 回 ENOTEMPTY（Finder 顯示 -47）。
// 做法和 lowntfs-3g 一樣：從頭列舉時把整個目錄讀成快照，之後的續傳都從快照取。
// cookie 的高 32 位元是快照編號、低 32 位元是快照內的索引。
// 快照列完就釋放；中途放棄的列舉由 LRU 或閒置逾時回收。
// 找不到快照（被擠掉）時重建一份、從同一個索引接著列，結果和舊做法一樣是盡力而為。
// 編號不會用到 0x7FFFFFFF，所以不會和 NTFSB_COOKIE_END 衝突。

static void ntfsb_dirsnap_free(ntfsb_dirsnap *s) {
  for (size_t i = 0; i < s->count; i++) {
    free(s->entries[i].name);
  }
  free(s->entries);
  memset(s, 0, sizeof(*s));
}

typedef struct ntfsb_dirsnap_builder {
  ntfsb_dirsnap_entry *entries;
  size_t count;
  size_t cap;
  bool nomem;
} ntfsb_dirsnap_builder;

static int ntfsb_dirsnap_collect(void *context, const char *name, size_t len, uint64_t mft,
                                 ntfsb_type type, int64_t cookie, int64_t next) {
  (void)cookie;
  (void)next;
  ntfsb_dirsnap_builder *b = context;
  if (b->count == b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 64;
    ntfsb_dirsnap_entry *e = realloc(b->entries, cap * sizeof(*e));
    if (!e) {
      b->nomem = true;
      return 1;
    }
    b->entries = e;
    b->cap = cap;
  }
  char *copy = malloc(len + 1);
  if (!copy) {
    b->nomem = true;
    return 1;
  }
  memcpy(copy, name, len);
  copy[len] = '\0';
  b->entries[b->count++] = (ntfsb_dirsnap_entry){ copy, len, mft, type };
  return 0;
}

static int64_t ntfsb_dirsnap_cookie(const ntfsb_dirsnap *s, size_t index) {
  return (int64_t)(((uint64_t)s->id << 32) | (uint64_t)index);
}

/// 建立 dir_mft_no 的快照並放進槽位。失敗回傳 NULL 並設定 *err。
static ntfsb_dirsnap *ntfsb_dirsnap_build(ntfsb_volume *v, uint64_t dir_mft_no, int *err) {
  ntfsb_dirsnap_builder b = { 0 };
  int64_t p = NTFSB_COOKIE_START;
  int rc = ntfsb_readdir_index(v, dir_mft_no, &p, ntfsb_dirsnap_collect, &b);
  if (!rc && b.nomem) {
    rc = -ENOMEM;
  }
  if (rc) {
    for (size_t i = 0; i < b.count; i++) {
      free(b.entries[i].name);
    }
    free(b.entries);
    *err = rc;
    return NULL;
  }
  // 空槽優先，否則擠掉最久沒用的（逾時的已在 ntfsb_readdir 開頭回收）
  ntfsb_dirsnap *slot = &v->snaps[0];
  for (int i = 0; i < NTFSB_DIRSNAP_SLOTS; i++) {
    if (!v->snaps[i].id) {
      slot = &v->snaps[i];
      break;
    }
    if (v->snaps[i].last_tick < slot->last_tick) {
      slot = &v->snaps[i];
    }
  }
  ntfsb_dirsnap_free(slot);
  if (++v->next_snap_id >= 0x7FFFFFFF) {
    v->next_snap_id = 1;
  }
  slot->id = v->next_snap_id;
  slot->dir_mft = dir_mft_no;
  slot->entries = b.entries;
  slot->count = b.count;
  return slot;
}

int ntfsb_readdir(ntfsb_volume *v,
                  uint64_t dir_mft_no,
                  int64_t *pos,
                  ntfsb_readdir_cb callback,
                  void *context) {
  if (!v || !v->vol || !pos || !callback) {
    return -EINVAL;
  }
  if (*pos >= NTFSB_COOKIE_END || *pos < 0) {
    return 0;
  }
  // 回收閒置太久的快照（中途放棄的列舉）
  time_t now = time(NULL);
  for (int i = 0; i < NTFSB_DIRSNAP_SLOTS; i++) {
    if (v->snaps[i].id && now - v->snaps[i].last_used > NTFSB_DIRSNAP_TTL) {
      ntfsb_dirsnap_free(&v->snaps[i]);
    }
  }
  ntfsb_dirsnap *s = NULL;
  size_t index = 0;
  if (*pos != NTFSB_COOKIE_START) {
    uint32_t id = (uint32_t)((uint64_t)*pos >> 32);
    index = (size_t)((uint64_t)*pos & 0xFFFFFFFFu);
    for (int i = 0; i < NTFSB_DIRSNAP_SLOTS && !s; i++) {
      if (v->snaps[i].id == id && v->snaps[i].dir_mft == dir_mft_no) {
        s = &v->snaps[i];
      }
    }
  }
  if (!s) {
    int err = 0;
    s = ntfsb_dirsnap_build(v, dir_mft_no, &err);
    if (!s) {
      return err;
    }
  }
  s->last_tick = ++v->snap_tick;
  s->last_used = now;
  for (size_t i = index; i < s->count; i++) {
    ntfsb_dirsnap_entry *e = &s->entries[i];
    int64_t next = i + 1 < s->count ? ntfsb_dirsnap_cookie(s, i + 1) : NTFSB_COOKIE_END;
    if (callback(context, e->name, e->len, e->mft, e->type, ntfsb_dirsnap_cookie(s, i), next)) {
      // 呼叫端拒收：下次從這個項目重新開始
      *pos = ntfsb_dirsnap_cookie(s, i);
      return 0;
    }
  }
  *pos = NTFSB_COOKIE_END;
  ntfsb_dirsnap_free(s);
  return 0;
}

// MARK: - 持久 handle

ntfsb_file *ntfsb_file_open(ntfsb_volume *v, uint64_t mft_no, int *out_err) {
  if (out_err) {
    *out_err = 0;
  }
  if (!v || !v->vol) {
    if (out_err) {
      *out_err = EINVAL;
    }
    return NULL;
  }
  // 同一個檔案被開啟多次時共用同一個 handle，確保只有一份 ntfs_inode
  ntfsb_file *existing = ntfsb_find_file(v, mft_no);
  if (existing) {
    existing->refs++;
    return existing;
  }

  ntfs_inode *ni = ntfs_inode_open(v->vol, (MFT_REF)mft_no);
  if (!ni) {
    if (out_err) {
      *out_err = errno ? errno : EIO;
    }
    return NULL;
  }
  int err = 0;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    err = EISDIR;
  } else if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    err = EOPNOTSUPP;
  }
  ntfs_attr *na = NULL;
  if (!err) {
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      err = errno ? errno : EIO;
    }
  }
  ntfsb_file *f = NULL;
  if (!err) {
    f = calloc(1, sizeof(*f));
    if (!f) {
      err = ENOMEM;
    }
  }
  if (err) {
    if (na) {
      ntfs_attr_close(na);
    }
    ntfs_inode_close(ni);
    if (out_err) {
      *out_err = err;
    }
    return NULL;
  }

  f->v = v;
  f->mft_no = mft_no;
  f->ni = ni;
  f->na = na;
  f->refs = 1;
  f->meta_dirty = false;
  f->next = v->open_files;
  v->open_files = f;
  return f;
}

/// 讓 handle 暫時放開它持有的 inode。
///
/// 有些 libntfs-3g 函式會「接管」傳入的 inode 並在內部關閉它——目前就是
/// ntfs_delete。若傳進去的是 handle 借出的 inode，handle 之後就指向已交還的
/// 記憶體，一旦 nidata 快取把它逐出釋放就是 use-after-free。
/// 呼叫端必須在這類操作前後成對呼叫 suspend / resume。
/// 回傳被暫停的 handle，沒有則回傳 NULL。
static ntfsb_file *ntfsb_file_suspend(ntfsb_volume *v, uint64_t mft_no) {
  ntfsb_file *f = ntfsb_find_file(v, mft_no);
  if (!f) {
    return NULL;
  }
  ntfs_attr_close(f->na);
  f->na = NULL;
  // 這裡要真的關閉（會順便寫回 dirty 的中繼資料）；接下來由呼叫端重新開一份
  ntfs_inode_close(f->ni);
  f->ni = NULL;
  f->meta_dirty = false;
  return f;
}

/// 與 ntfsb_file_suspend 配對，f 為 suspend 的回傳值。
/// 檔案已經不存在時 handle 會維持暫停狀態，後續讀寫回傳 -ESTALE，直到呼叫端關閉它；
/// 同時讓它脫離原本的 MFT 編號：該記錄之後可能被新檔案或目錄重複使用，
/// 不能讓 ntfsb_find_file_any 或下一次 resume 把舊 handle 接到新的 inode 上。
static void ntfsb_file_resume(ntfsb_volume *v, ntfsb_file *f) {
  if (!f || f->ni) {
    return;
  }
  ntfs_inode *ni = ntfs_inode_open(v->vol, (MFT_REF)f->mft_no);
  // 最後一個名稱被移除後 MFT 記錄會被標為未使用，這種情況不要再接上去
  if (ni && !(ni->mrec->flags & MFT_RECORD_IN_USE)) {
    ntfs_inode_close(ni);
    ni = NULL;
  }
  ntfs_attr *na = ni ? ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0) : NULL;
  if (!na) {
    if (ni) {
      ntfs_inode_close(ni);
    }
    f->mft_no = NTFSB_MFT_GONE;
    return;
  }
  f->ni = ni;
  f->na = na;
}

int64_t ntfsb_file_read(ntfsb_file *f, int64_t offset, void *buf, size_t len) {
  if (!f || !buf || offset < 0) {
    return -EINVAL;
  }
  if (!f->na) {
    return -ESTALE;  // 檔案已被刪除
  }
  if (len == 0) {
    return 0;
  }
  s64 max_read = f->na->data_size;
  if (offset >= max_read) {
    return 0;
  }
  s64 size = (s64)len;
  if (offset + size > max_read) {
    size = max_read - offset;
  }
  s64 total = 0;
  while (size > 0) {
    s64 ret = ntfs_attr_pread(f->na, offset + total, size, (uint8_t *)buf + total);
    if (ret <= 0 || ret > size) {
      return (ret < 0) ? ntfsb_neg_errno() : -EIO;
    }
    size -= ret;
    total += ret;
  }
  return total;
}

int64_t ntfsb_file_write(ntfsb_file *f, int64_t offset, const void *buf, size_t len) {
  if (!f || !buf || offset < 0) {
    return -EINVAL;
  }
  if (NVolReadOnly(f->v->vol)) {
    return -EROFS;
  }
  if (!f->na) {
    return -ESTALE;  // 檔案已被刪除
  }
  if (len == 0) {
    return 0;
  }
  s64 size = (s64)len;
  s64 total = 0;
  while (size > 0) {
    s64 ret = ntfs_attr_pwrite(f->na, offset + total, size, (const uint8_t *)buf + total);
    if (ret <= 0) {
      return ntfsb_neg_errno();
    }
    size -= ret;
    total += ret;
  }
  f->ni->flags |= FILE_ATTR_ARCHIVE;
  // 只更新記憶體中的時間戳（不做 I/O）；MFT record 的寫回延後到 flush 或 close。
  ntfs_inode_update_times(f->ni, NTFS_UPDATE_MCTIME);
  f->meta_dirty = true;
  return total;
}

int ntfsb_file_flush(ntfsb_file *f) {
  if (!f) {
    return -EINVAL;
  }
  if (!f->meta_dirty || !f->ni) {
    return 0;
  }
  if (ntfs_inode_sync(f->ni)) {
    return ntfsb_neg_errno();
  }
  f->meta_dirty = false;
  return 0;
}

int ntfsb_file_close(ntfsb_file *f) {
  if (!f) {
    return -EINVAL;
  }
  if (--f->refs > 0) {
    return 0;
  }
  ntfsb_volume *v = f->v;
  // 先從串列移除，後面的 ntfs_inode_close 才不會被 ntfsb_close_inode 攔下來
  ntfsb_file **pp = &v->open_files;
  while (*pp && *pp != f) {
    pp = &(*pp)->next;
  }
  if (*pp) {
    *pp = f->next;
  }
  int res = 0;
  // 暫停中的 handle 已經沒有 inode 與 attr 可關（檔案被刪掉了）
  if (f->na) {
    ntfs_attr_close(f->na);
  }
  // ntfs_inode_close 內部會在 dirty 時自行 ntfs_inode_sync
  if (f->ni && ntfs_inode_close(f->ni)) {
    res = ntfsb_neg_errno();
  }
  free(f);
  return res;
}

int ntfsb_flush_all(ntfsb_volume *v) {
  if (!v || !v->vol) {
    return -EINVAL;
  }
  int res = 0;
  for (ntfsb_file *f = v->open_files; f; f = f->next) {
    int rc = ntfsb_file_flush(f);
    if (rc && !res) {
      res = rc;
    }
  }
  return res;
}

bool ntfsb_has_dirty_files(const ntfsb_volume *v) {
  if (!v) {
    return false;
  }
  for (const ntfsb_file *f = v->open_files; f; f = f->next) {
    if (f->meta_dirty) {
      return true;
    }
  }
  return false;
}

// MARK: - 讀寫

int64_t ntfsb_read(ntfsb_volume *v, uint64_t mft_no, int64_t offset, void *buf, size_t len) {
  if (!v || !v->vol || !buf || offset < 0) {
    return -EINVAL;
  }
  if (len == 0) {
    return 0;
  }
  ntfsb_file *held = ntfsb_find_file(v, mft_no);
  if (held) {
    return ntfsb_file_read(held, offset, buf, len);
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int64_t res;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    res = -EISDIR;
    goto exit;
  }
  if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto exit;
  }
  {
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    s64 max_read = na->data_size;
    s64 size = (s64)len;
    if (offset >= max_read) {
      res = 0;
      ntfs_attr_close(na);
      goto exit;
    }
    if (offset + size > max_read) {
      size = max_read - offset;
    }
    s64 total = 0;
    while (size > 0) {
      s64 ret = ntfs_attr_pread(na, offset + total, size, (uint8_t *)buf + total);
      if (ret <= 0 || ret > size) {
        res = (ret < 0) ? ntfsb_neg_errno() : -EIO;
        ntfs_attr_close(na);
        goto exit;
      }
      size -= ret;
      total += ret;
    }
    ntfs_attr_close(na);
    res = total;
  }
exit:
  if (ntfsb_close_inode(v, ni) && res >= 0) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int64_t ntfsb_write(ntfsb_volume *v, uint64_t mft_no, int64_t offset, const void *buf, size_t len) {
  if (!v || !v->vol || !buf || offset < 0) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  if (len == 0) {
    return 0;
  }
  ntfsb_file *held = ntfsb_find_file(v, mft_no);
  if (held) {
    return ntfsb_file_write(held, offset, buf, len);
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int64_t res;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    res = -EISDIR;
    goto exit;
  }
  if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto exit;
  }
  {
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    s64 size = (s64)len;
    s64 total = 0;
    while (size > 0) {
      s64 ret = ntfs_attr_pwrite(na, offset + total, size, (const uint8_t *)buf + total);
      if (ret <= 0) {
        res = ntfsb_neg_errno();
        ntfs_attr_close(na);
        goto exit;
      }
      size -= ret;
      total += ret;
    }
    ntfs_attr_close(na);
    res = total;
    ni->flags |= FILE_ATTR_ARCHIVE;
    ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
  }
exit:
  if (ntfsb_close_inode(v, ni) && res >= 0) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_truncate(ntfsb_volume *v, uint64_t mft_no, int64_t size) {
  if (!v || !v->vol || size < 0) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (ni->mft_no < FILE_first_user) {
    res = -EPERM;
    goto exit;
  }
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    res = -EISDIR;
    goto exit;
  }
  if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto exit;
  }
  {
    // 有 handle 時必須沿用 handle 的 ntfs_attr，不能另開一份，
    // 否則兩份 runlist 會分歧
    ntfsb_file *held = ntfsb_find_file(v, mft_no);
    ntfs_attr *na = held ? held->na : ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    s64 oldsize = na->data_size;
    if ((na->data_flags & ATTR_COMPRESSION_MASK) && (size > na->initialized_size)) {
      // 壓縮檔放大：寫入一個結尾零位元組，讓函式庫建立 hole
      char zero = 0;
      if (ntfs_attr_pwrite(na, size - 1, 1, &zero) <= 0) {
        res = ntfsb_neg_errno();
      }
    } else if ((size > na->data_size && !(na->data_flags & ATTR_IS_SPARSE))
                   ? ntfs_attr_truncate_solid(na, size)
                   : ntfs_attr_truncate(na, size)) {
      // 拉長時真的配置叢集（與 Windows SetEndOfFile 一致）而不是留洞：
      // 留洞會把檔案標成 sparse，之後就不能走 kernel offloaded I/O。
      // 本來就是 sparse 的檔案維持原本留洞的行為。
      res = ntfsb_neg_errno();
    }
    if (!held) {
      ntfs_attr_close(na);
    }
    if (!res) {
      if (oldsize != size) {
        ni->flags |= FILE_ATTR_ARCHIVE;
      }
      ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
      if (held) {
        held->meta_dirty = true;
      }
    }
  }
exit:
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

// MARK: - 建立 / 刪除 / 連結 / 重新命名

int ntfsb_create(ntfsb_volume *v,
                 uint64_t dir_mft_no,
                 const char *name,
                 ntfsb_type type,
                 const char *symlink_target,
                 uint64_t *out_mft_no) {
  if (!v || !v->vol || !name || !out_mft_no) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  mode_t mode;
  switch (type) {
    case NTFSB_TYPE_FILE: mode = S_IFREG; break;
    case NTFSB_TYPE_DIR: mode = S_IFDIR; break;
    case NTFSB_TYPE_SYMLINK: mode = S_IFLNK; break;
    case NTFSB_TYPE_FIFO: mode = S_IFIFO; break;
    case NTFSB_TYPE_SOCK: mode = S_IFSOCK; break;
    default: return -EINVAL;
  }
  if (type == NTFSB_TYPE_SYMLINK && !symlink_target) {
    return -EINVAL;
  }
  ntfschar *uname = NULL;
  int uname_len = ntfsb_name_to_ucs(name, &uname);
  if (uname_len < 0) {
    return uname_len;
  }
  if (ntfsb_name_is_forbidden(uname, uname_len)) {
    free(uname);
    return -EINVAL;
  }
  ntfs_inode *dir_ni = ntfsb_open_inode(v, dir_mft_no);
  if (!dir_ni) {
    free(uname);
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (dir_ni->mft_no == FILE_Extend) {
    res = -EPERM;
    goto exit;
  }
  if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
    res = -ENOTDIR;
    goto exit;
  }
  if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto exit;
  }
  {
    ntfs_inode *ni = NULL;
    if (type == NTFSB_TYPE_SYMLINK) {
      ntfschar *utarget = NULL;
      int utarget_len = ntfs_mbstoucs(symlink_target, &utarget);
      if (utarget_len < 0) {
        res = ntfsb_neg_errno();
        goto exit;
      }
      ni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0), uname, (u8)uname_len,
                               utarget, utarget_len);
      free(utarget);
    } else {
      ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)uname_len, mode);
    }
    if (!ni) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    ni->flags |= FILE_ATTR_ARCHIVE;
    NInoSetDirty(ni);
    *out_mft_no = ni->mft_no;
    // libntfs-3g 的 lookup 快取連「找不到」也會記住，所以定義了新名稱之後
    // 一定要更新該筆記錄，否則先前的負向結果會讓剛建立的檔案查不到。
    // （ntfs_delete 會自行失效，只有建立名稱這一側要呼叫端處理。）
    ntfsb_update_mbsname(dir_ni, name, ni->mft_no);
    // 關閉 ni 需要同步父目錄索引，因此用 close_in_dir 避免重複開啟
    if (ntfs_inode_close_in_dir(ni, dir_ni)) {
      res = ntfsb_neg_errno();
    }
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
  }
exit:
  free(uname);
  if (ntfsb_close_inode(v, dir_ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

bool ntfsb_inode_exists(ntfsb_volume *v, uint64_t mft_no) {
  if (!v || !v->vol) {
    return false;
  }
  if (ntfsb_find_file(v, mft_no)) {
    return true;
  }
  ntfs_inode *ni = ntfs_inode_open(v->vol, (MFT_REF)mft_no);
  if (!ni) {
    return false;
  }
  bool in_use = (ni->mrec->flags & MFT_RECORD_IN_USE) != 0;
  ntfs_inode_close(ni);
  return in_use;
}

int ntfsb_unlink(ntfsb_volume *v, uint64_t dir_mft_no, const char *name, uint64_t mft_no) {
  if (!v || !v->vol || !name) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  if (mft_no < FILE_first_user) {
    return -EPERM;
  }
  ntfschar *uname = NULL;
  int uname_len = ntfsb_name_to_ucs(name, &uname);
  if (uname_len < 0) {
    return uname_len;
  }
  int res = 0;
  ntfs_inode *ni = NULL;
  ntfs_inode *dir_ni = NULL;
  // ntfs_delete 會接管並關閉傳入的 inode，所以不能把 handle 借出的那一份交給它。
  // 先讓 handle 放手，等這裡做完再接回來。
  ntfsb_file *suspended = ntfsb_file_suspend(v, mft_no);

  ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    res = ntfsb_neg_errno();
    goto done;
  }
  dir_ni = ntfsb_open_inode(v, dir_mft_no);
  if (!dir_ni) {
    res = ntfsb_neg_errno();
    goto done;
  }
  if (dir_ni->mft_no == FILE_Extend) {
    res = -EPERM;
    goto done;
  }
  // 非空目錄的判斷交給 ntfs_delete 內部的 ntfs_check_unlinkable_dir：
  // 它在動到索引之前檢查，且允許移除「有兩個以上連結」的非空目錄的其中一個名稱。
  // rename 是先 link 再 unlink，這裡若自行用 ntfs_check_empty_dir 擋掉，
  // 搬移非空目錄（例如丟到垃圾桶）時舊名稱刪不掉、回復也失敗，目錄會同時掛在兩個位置。
  if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto done;
  }
  // ntfs_delete 無論成功與否都會關閉 ni 與 dir_ni
  if (ntfs_delete(v->vol, NULL, ni, dir_ni, uname, (u8)uname_len)) {
    res = ntfsb_neg_errno();
  }
  ni = NULL;
  dir_ni = NULL;

done:
  if (dir_ni && ntfsb_close_inode(v, dir_ni) && !res) {
    res = ntfsb_neg_errno();
  }
  if (ni && ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  // 檔案還在（例如還有其他硬連結，或這是 rename 的前半段）就接回去；
  // 已經消失則 handle 維持暫停，後續讀寫回傳 ESTALE。
  ntfsb_file_resume(v, suspended);
  free(uname);
  return res;
}

int ntfsb_link(ntfsb_volume *v, uint64_t mft_no, uint64_t dir_mft_no, const char *name) {
  if (!v || !v->vol || !name) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfschar *uname = NULL;
  int uname_len = ntfsb_name_to_ucs(name, &uname);
  if (uname_len < 0) {
    return uname_len;
  }
  if (ntfsb_name_is_forbidden(uname, uname_len)) {
    free(uname);
    return -EINVAL;
  }
  int res = 0;
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    free(uname);
    return ntfsb_neg_errno();
  }
  ntfs_inode *dir_ni = ntfsb_open_inode(v, dir_mft_no);
  if (!dir_ni) {
    res = ntfsb_neg_errno();
    ntfsb_close_inode(v, ni);
    free(uname);
    return res;
  }
  if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
  } else if (ntfs_link(ni, dir_ni, uname, (u8)uname_len)) {
    res = ntfsb_neg_errno();
  } else {
    // 同 ntfsb_create：新名稱必須寫進 lookup 快取，蓋掉可能存在的負向記錄
    ntfsb_update_mbsname(dir_ni, name, ni->mft_no);
    ni->flags |= FILE_ATTR_ARCHIVE;
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
  }
  // 必須先關閉 dir_ni，否則 ni 的檔名同步可能找不到索引項目
  if (ntfsb_close_inode(v, dir_ni) && !res) {
    res = ntfsb_neg_errno();
  }
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  free(uname);
  return res;
}

int ntfsb_rename(ntfsb_volume *v,
                 uint64_t src_dir_mft_no,
                 const char *src_name,
                 uint64_t mft_no,
                 uint64_t dst_dir_mft_no,
                 const char *dst_name,
                 uint64_t over_mft_no) {
  if (!v || !v->vol || !src_name || !dst_name) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  if (over_mft_no == mft_no) {
    // 同一個 inode：視為無動作
    return 0;
  }
  int res;
  if (over_mft_no) {
    res = ntfsb_unlink(v, dst_dir_mft_no, dst_name, over_mft_no);
    if (res) {
      return res;
    }
  }
  res = ntfsb_link(v, mft_no, dst_dir_mft_no, dst_name);
  if (res) {
    return res;
  }
  res = ntfsb_unlink(v, src_dir_mft_no, src_name, mft_no);
  if (res) {
    // 回復：移除剛建立的新名稱
    ntfsb_unlink(v, dst_dir_mft_no, dst_name, mft_no);
  }
  return res;
}

int ntfsb_readlink(ntfsb_volume *v, uint64_t mft_no, char *buf, size_t bufsize) {
  if (!v || !v->vol || !buf || bufsize == 0) {
    return -EINVAL;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  if (ni->flags & FILE_ATTR_REPARSE_POINT) {
    errno = 0;
    char *target = ntfs_make_symlink(ni, v->mnt_point);
    if (target) {
      res = (int)strlcpy(buf, target, bufsize);
      free(target);
    } else if (errno == EOPNOTSUPP || errno == 0) {
      res = (int)strlcpy(buf, NTFSB_BAD_REPARSE, bufsize);
    } else {
      res = ntfsb_neg_errno();
    }
    goto exit;
  }
  if (!(ni->flags & FILE_ATTR_SYSTEM)) {
    res = -EINVAL;
    goto exit;
  }
  {
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    if ((size_t)na->data_size <= sizeof(INTX_FILE_TYPES)) {
      res = -EINVAL;
      ntfs_attr_close(na);
      goto exit;
    }
    if ((size_t)na->data_size > sizeof(INTX_FILE_TYPES) + sizeof(ntfschar) * PATH_MAX) {
      res = -ENAMETOOLONG;
      ntfs_attr_close(na);
      goto exit;
    }
    INTX_FILE *intx = ntfs_malloc(na->data_size);
    if (!intx) {
      res = -ENOMEM;
      ntfs_attr_close(na);
      goto exit;
    }
    if (ntfs_attr_pread(na, 0, na->data_size, intx) != na->data_size) {
      res = ntfsb_neg_errno();
    } else if (intx->magic != INTX_SYMBOLIC_LINK) {
      res = -EINVAL;
    } else {
      char *target = NULL;
      int len = ntfs_ucstombs(intx->target,
                              (int)((na->data_size - offsetof(INTX_FILE, target)) / sizeof(ntfschar)),
                              &target, 0);
      if (len < 0) {
        res = ntfsb_neg_errno();
      } else {
        res = (int)strlcpy(buf, target, bufsize);
        free(target);
      }
    }
    free(intx);
    ntfs_attr_close(na);
  }
exit:
  if (ntfsb_close_inode(v, ni) && res >= 0) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_set_times(ntfsb_volume *v,
                    uint64_t mft_no,
                    const struct timespec *atime,
                    const struct timespec *mtime,
                    const struct timespec *ctime,
                    const struct timespec *btime) {
  if (!v || !v->vol) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  if (!atime && !mtime && !ctime && !btime) {
    return 0;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  if (atime) {
    ni->last_access_time = timespec2ntfs(*atime);
  }
  if (mtime) {
    ni->last_data_change_time = timespec2ntfs(*mtime);
  }
  if (btime) {
    ni->creation_time = timespec2ntfs(*btime);
  }
  if (ctime) {
    ni->last_mft_change_time = timespec2ntfs(*ctime);
    NInoFileNameSetDirty(ni);
    NInoSetDirty(ni);
  } else {
    // 明確設定時間時仍更新 ctime，並同步檔名索引中的時間戳
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
  }
  int res = 0;
  if (ntfsb_close_inode(v, ni)) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_set_flags(ntfsb_volume *v, uint64_t mft_no, int set_readonly, int set_hidden) {
  if (!v || !v->vol) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  if (set_readonly < 0 && set_hidden < 0) {
    return 0;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  if (set_readonly == 0) {
    ni->flags &= ~FILE_ATTR_READONLY;
  } else if (set_readonly > 0) {
    ni->flags |= FILE_ATTR_READONLY;
  }
  if (set_hidden == 0) {
    ni->flags &= ~FILE_ATTR_HIDDEN;
  } else if (set_hidden > 0) {
    ni->flags |= FILE_ATTR_HIDDEN;
  }
  NInoFileNameSetDirty(ni);
  ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
  int res = 0;
  if (ntfsb_close_inode(v, ni)) {
    res = ntfsb_neg_errno();
  }
  return res;
}

// MARK: - 延伸屬性（具名 $DATA 串流）

int ntfsb_xattr_list(ntfsb_volume *v, uint64_t mft_no, ntfsb_xattr_list_cb callback, void *context) {
  if (!v || !v->vol || !callback) {
    return -EINVAL;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  ntfs_attr_search_ctx *actx = ntfs_attr_get_search_ctx(ni, NULL);
  if (!actx) {
    res = ntfsb_neg_errno();
    goto exit;
  }
  while (!ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, actx)) {
    ATTR_RECORD *attr = actx->attr;
    if (!attr->name_length) {
      continue;
    }
    const ntfschar *uname = (const ntfschar *)((const u8 *)attr + le16_to_cpu(attr->name_offset));
    char *name = NULL;
    int len = ntfs_ucstombs(uname, attr->name_length, &name, 0);
    if (len < 0) {
      continue;
    }
    int stop = callback(context, name, (size_t)len);
    free(name);
    if (stop) {
      break;
    }
  }
  if (errno != ENOENT && errno != 0) {
    // ntfs_attr_lookup 以 ENOENT 表示列舉結束
  }
  ntfs_attr_put_search_ctx(actx);
exit:
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

/// 開啟具名串流，不存在時回傳 NULL 並設定 errno = ENOATTR
static ntfs_attr *ntfsb_open_stream(ntfs_inode *ni, const char *name, ntfschar **out_uname, int *out_len) {
  ntfschar *uname = NULL;
  int len = ntfs_mbstoucs(name, &uname);
  if (len < 0) {
    return NULL;
  }
  if (len == 0 || len > NTFS_MAX_NAME_LEN) {
    free(uname);
    errno = ENAMETOOLONG;
    return NULL;
  }
  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, uname, len);
  if (!na) {
    if (errno == ENOENT) {
      errno = ENOATTR;
    }
    if (out_uname) {
      // 呼叫端可能需要用名稱建立串流
      *out_uname = uname;
      *out_len = len;
      return NULL;
    }
    free(uname);
    return NULL;
  }
  if (out_uname) {
    *out_uname = uname;
    *out_len = len;
  } else {
    free(uname);
  }
  return na;
}

int64_t ntfsb_xattr_get(ntfsb_volume *v, uint64_t mft_no, const char *name, void *buf, size_t bufsize) {
  if (!v || !v->vol || !name) {
    return -EINVAL;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int64_t res;
  ntfs_attr *na = ntfsb_open_stream(ni, name, NULL, NULL);
  if (!na) {
    res = ntfsb_neg_errno();
    goto exit;
  }
  res = na->data_size;
  if (buf) {
    if ((size_t)na->data_size > bufsize) {
      res = -ERANGE;
    } else if (na->data_size > 0) {
      s64 got = ntfs_attr_pread(na, 0, na->data_size, buf);
      if (got != na->data_size) {
        res = got < 0 ? ntfsb_neg_errno() : -EIO;
      }
    }
  }
  ntfs_attr_close(na);
exit:
  if (ntfsb_close_inode(v, ni) && res >= 0) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_xattr_set(ntfsb_volume *v,
                    uint64_t mft_no,
                    const char *name,
                    const void *data,
                    size_t size,
                    ntfsb_xattr_policy policy) {
  if (!v || !v->vol || !name || (!data && size)) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  ntfschar *uname = NULL;
  int uname_len = 0;
  if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
    // NTFS 目錄也能有具名串流，但為避免與索引屬性混淆，這裡仍允許
  }
  ntfs_attr *na = ntfsb_open_stream(ni, name, &uname, &uname_len);
  if (!na) {
    if (errno != ENOATTR) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    if (policy == NTFSB_XATTR_MUST_REPLACE) {
      res = -ENOATTR;
      goto exit;
    }
    if (ntfsb_name_is_forbidden(uname, uname_len)) {
      res = -EINVAL;
      goto exit;
    }
    if (ntfs_attr_add(ni, AT_DATA, uname, uname_len, NULL, 0)) {
      res = ntfsb_neg_errno();
      goto exit;
    }
    na = ntfs_attr_open(ni, AT_DATA, uname, uname_len);
    if (!na) {
      res = ntfsb_neg_errno();
      goto exit;
    }
  } else if (policy == NTFSB_XATTR_MUST_CREATE) {
    res = -EEXIST;
    ntfs_attr_close(na);
    goto exit;
  }
  if (ntfs_attr_truncate(na, 0)) {
    res = ntfsb_neg_errno();
  } else if (size > 0) {
    s64 put = ntfs_attr_pwrite(na, 0, (s64)size, data);
    if (put != (s64)size) {
      res = put < 0 ? ntfsb_neg_errno() : -EIO;
    }
  }
  ntfs_attr_close(na);
  if (!res) {
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
  }
exit:
  free(uname);
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

int ntfsb_xattr_remove(ntfsb_volume *v, uint64_t mft_no, const char *name) {
  if (!v || !v->vol || !name) {
    return -EINVAL;
  }
  if (NVolReadOnly(v->vol)) {
    return -EROFS;
  }
  ntfs_inode *ni = ntfsb_open_inode(v, mft_no);
  if (!ni) {
    return ntfsb_neg_errno();
  }
  int res = 0;
  ntfs_attr *na = ntfsb_open_stream(ni, name, NULL, NULL);
  if (!na) {
    res = ntfsb_neg_errno();
    goto exit;
  }
  // ntfs_attr_rm 成功時會一併關閉 na
  if (ntfs_attr_rm(na)) {
    res = ntfsb_neg_errno();
    ntfs_attr_close(na);
  } else {
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
  }
exit:
  if (ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
  return res;
}

// MARK: - Kernel offloaded I/O

bool ntfsb_koio_eligible(const ntfsb_attr *a, uint32_t cluster_size) {
  if (!a || a->type != NTFSB_TYPE_FILE || cluster_size == 0) {
    return false;
  }
  if (a->compressed || a->encrypted || a->sparse) {
    return false;
  }
  // 常駐檔案的資料在 MFT record 裡，沒有叢集可映射；但空檔案例外——
  // kernel 只在 lookup / create / getattr 時取得這個旗標，寫入途中不會再問，
  // 新檔案若一開始就 inhibit，整個檔案都會走慢路徑。空檔案交給 blockmap 時再轉成非常駐。
  if (a->resident && a->size > 0) {
    return false;
  }
  if (a->init_size < a->size && (a->init_size % cluster_size) != 0) {
    return false;
  }
  return true;
}

/// 取得裝置層的區塊快取（供 blockmap 做範圍 flush）
static ntfsb_bcache *ntfsb_volume_bcache(ntfsb_volume *v) {
  ntfsb_dev_priv *p = ntfsb_dev(v->vol->dev);
  return p ? p->cache : NULL;
}

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

/// 把 [from, to) 覆寫成零。呼叫端必須保證 to <= initialized_size，
/// 否則 ntfs_attr_pwrite 會順便把 [initialized_size, from) 也補零。
static int ntfsb_attr_zero_range(ntfs_attr *na, int64_t from, int64_t to) {
  enum { ZERO_CHUNK = 256 * 1024 };
  if (to > na->initialized_size) {
    errno = EINVAL;
    return -1;
  }
  uint8_t *zeros = calloc(1, ZERO_CHUNK);
  if (!zeros) {
    errno = ENOMEM;
    return -1;
  }
  int res = 0;
  for (int64_t pos = from; pos < to;) {
    int64_t n = to - pos;
    if (n > ZERO_CHUNK) {
      n = ZERO_CHUNK;
    }
    s64 w = ntfs_attr_pwrite(na, pos, n, zeros);
    if (w <= 0) {
      res = -1;
      break;
    }
    pos += w;
  }
  free(zeros);
  return res;
}

/// 把 [start, end) 依 runlist 打包成 Data／ZeroFill extent。
/// handed_end（可為 NULL）回傳交給 cb 的最後一個 extent 的結尾；
/// FSExtentPacker 回報已滿時，該 extent 本身仍已收下。
/// 回傳 0、1（packer 已滿）或負 errno。
static int ntfsb_pack_runs(ntfsb_volume *v, ntfs_attr *na, int64_t start, int64_t end,
                           ntfsb_extent_cb cb, void *context, int64_t *handed_end) {
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
      if (handed_end) {
        *handed_end = gap.logical + gap.length;
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
    if (handed_end) {
      *handed_end = e.logical + e.length;
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
    if (!NAttrNonResident(na) && !NVolReadOnly(v->vol) && (for_write || na->data_size > 0)) {
      // 常駐屬性沒有叢集：轉成非常駐才有東西可以交給 kernel。
      // 正常只會在寫入空檔案時遇到（見 ntfsb_koio_eligible）；讀取常駐且有內容的檔案
      // 代表 kernel 的旗標過期，轉換一樣正確，只是多占一個叢集。讀取空檔案不必轉。
      if (ntfs_attr_force_non_resident(na)) {
        res = ntfsb_neg_errno();
        if (held) {
          held->meta_dirty = true;
        }
        goto close_attr;
      }
      if (held) {
        held->meta_dirty = true;
      }
    }
    if (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED)) {
      res = -EINVAL;
    } else if (!NAttrNonResident(na)) {
      // 沒轉換的常駐檔案：只有空檔案會被判定合格，整段以零呈現
      if (na->data_size == 0) {
        ntfsb_extent z = {offset, 0, length, true};
        cb(context, &z);
      } else {
        res = -EINVAL;
      }
    } else if (for_write) {
      int64_t end = offset + length;
      int rc = 0;
      // 1. 確保 [offset, end) 有叢集：拉長時真的配置，不留洞
      if (end > na->data_size && ntfs_attr_truncate_solid(na, end)) {
        rc = ntfsb_neg_errno();
      }
      // 2. kernel 只會寫 [offset, end)，中間的洞要先在磁碟上補零。
      //    步驟 4 保證 initialized_size 涵蓋所有交給 kernel 的範圍，
      //    所以這裡補的一定是真正沒人寫過的洞，不會蓋掉 kernel 還沒 complete 的寫入。
      if (!rc && offset > na->initialized_size && ntfsb_attr_zero_to(na, offset)) {
        rc = ntfsb_neg_errno();
      }
      // 3. 打包
      int64_t handed_end = 0;
      if (!rc) {
        rc = ntfsb_pack_runs(v, na, offset, end, cb, context, &handed_end);
      }
      // 4. 交出去的範圍立刻算進 initialized_size，不等 complete_io。
      //    kernel 會把同一個檔案拆成多段、各自 blockmap，前段寫進磁碟後
      //    complete_io 未必先回來；若 initialized_size 還停在前段之前，
      //    後段的步驟 2 就會把前段當成洞補零（Finder 複製時檔案前段全變成零）。
      //    代價是寫入失敗時要在 complete_io 把該段改回零，見 ntfsb_complete_io。
      if (handed_end > end) {
        handed_end = end;  // 叢集對齊多出來的部分 kernel 不會寫
      }
      if (rc >= 0 && handed_end > na->initialized_size && ntfsb_attr_set_initialized(na, handed_end)) {
        rc = ntfsb_neg_errno();
      }
      if (rc >= 0) {
        ni->flags |= FILE_ATTR_ARCHIVE;
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
        if (held) {
          held->meta_dirty = true;
        }
      }
      res = rc < 0 ? rc : 0;
    } else {
      int64_t end = offset + length;
      int64_t init = na->initialized_size;
      int rc = 0;
      if (offset < init) {
        rc = ntfsb_pack_runs(v, na, offset, end < init ? end : init, cb, context, NULL);
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
close_attr:
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
      // blockmap 已經推進過 initialized_size，正常情況這裡不會再改動
      if (end > na->data_size && ntfs_attr_truncate_solid(na, end)) {
        res = ntfsb_neg_errno();
      } else if (end > na->initialized_size && ntfsb_attr_set_initialized(na, end)) {
        res = ntfsb_neg_errno();
      }
    } else if (offset < na->initialized_size) {
      // 失敗的寫入：blockmap 已把這段算進 initialized_size，磁碟上卻可能是舊資料，
      // 必須讓兩條路徑都讀成零。
      if (end >= na->initialized_size) {
        // 檔尾那段：把 initialized_size 退回 offset 即可，不必再碰（可能正在出錯的）裝置。
        // initialized_size 涵蓋所有交出去的範圍，所以 end 之後沒有其他寫入在途中。
        if (ntfsb_attr_set_initialized(na, offset)) {
          res = ntfsb_neg_errno();
        }
      } else if (ntfsb_attr_zero_range(na, offset, end)) {
        // 中間那段：後面已有資料，只能把這段覆寫成零
        res = ntfsb_neg_errno();
      }
    }
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
