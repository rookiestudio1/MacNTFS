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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

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
struct ntfsb_volume {
  ntfs_volume *vol;
  char *label;        ///< UTF-8 卷標快取
  char *mnt_point;    ///< 解析 junction 時使用的掛載點
  ntfsb_file *open_files;  ///< 目前持有的持久 handle（見下方說明）
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

struct ntfsb_file {
  ntfsb_file *next;
  ntfsb_volume *v;
  uint64_t mft_no;
  ntfs_inode *ni;
  ntfs_attr *na;
  int refs;          ///< 同一個檔案被開啟多次時共用同一個 handle
  bool meta_dirty;   ///< 有尚未寫回的中繼資料（大小、時間戳）
};

/// 找出 mft_no 對應的 handle，沒有則回傳 NULL
static ntfsb_file *ntfsb_find_file(ntfsb_volume *v, uint64_t mft_no) {
  for (ntfsb_file *f = v->open_files; f; f = f->next) {
    if (f->mft_no == mft_no) {
      return f;
    }
  }
  return NULL;
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
    inum = ntfs_inode_lookup_by_mbsname(dir_ni, name);
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

int ntfsb_readdir(ntfsb_volume *v,
                  uint64_t dir_mft_no,
                  int64_t *pos,
                  ntfsb_readdir_cb callback,
                  void *context) {
  if (!v || !v->vol || !pos || !callback) {
    return -EINVAL;
  }
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

int64_t ntfsb_file_read(ntfsb_file *f, int64_t offset, void *buf, size_t len) {
  if (!f || !buf || offset < 0) {
    return -EINVAL;
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
  if (!f->meta_dirty) {
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
  ntfs_attr_close(f->na);
  // ntfs_inode_close 內部會在 dirty 時自行 ntfs_inode_sync
  if (ntfs_inode_close(f->ni)) {
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
    } else if (ntfs_attr_truncate(na, size)) {
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
    ntfs_inode_update_mbsname(dir_ni, name, ni->mft_no);
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
  if (dir_ni->mft_no == FILE_Extend) {
    res = -EPERM;
    goto exit_close;
  }
  if ((ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) && ntfs_check_empty_dir(ni)) {
    res = ntfsb_neg_errno();
    goto exit_close;
  }
  if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
    res = -EOPNOTSUPP;
    goto exit_close;
  }
  // ntfs_delete 無論成功與否都會關閉 ni 與 dir_ni
  if (ntfs_delete(v->vol, NULL, ni, dir_ni, uname, (u8)uname_len)) {
    res = ntfsb_neg_errno();
  }
  ni = NULL;
  dir_ni = NULL;
exit_close:
  if (dir_ni && ntfsb_close_inode(v, dir_ni) && !res) {
    res = ntfsb_neg_errno();
  }
  if (ni && ntfsb_close_inode(v, ni) && !res) {
    res = ntfsb_neg_errno();
  }
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
    ntfs_inode_update_mbsname(dir_ni, name, ni->mft_no);
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
