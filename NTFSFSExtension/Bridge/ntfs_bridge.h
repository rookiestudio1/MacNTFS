//
//  ntfs_bridge.h
//  NTFSFSExtension
//
//  libntfs-3g 的純 C 橋接層。
//  設計原則：
//  - 對外只暴露不透明的 ntfsb_volume，以 MFT 編號（mft_no）作為檔案身分。
//  - 預設每個操作內部自行 ntfs_inode_open / ntfs_inode_close，不長期持有 ntfs_inode。
//    需要跨多次讀寫時改用 ntfsb_file handle（見下方「持久 handle」）；handle 存在期間
//    橋接層會讓所有針對該 mft_no 的操作共用同一份 ntfs_inode，因為 libntfs-3g 的
//    ntfs_inode_open 沒有引用計數，重複開啟會得到兩份互相覆寫的 MFT 記錄。
//  - 所有函式回傳 0 表示成功、負 errno 表示失敗（讀寫類回傳位元組數或負 errno）。
//  - libntfs-3g 並非執行緒安全，呼叫端必須自行序列化對同一 ntfsb_volume 的呼叫。
//

#ifndef NTFS_BRIDGE_H
#define NTFS_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 不透明的已掛載卷
typedef struct ntfsb_volume ntfsb_volume;

/// 不透明的持久檔案 handle
typedef struct ntfsb_file ntfsb_file;

/// 呼叫端提供的區塊裝置存取介面。
/// pread/pwrite 收到的 offset 與 count 一定是 block_size 的整數倍（橋接層會處理對齊）。
/// 回傳實際處理的位元組數；失敗回傳 -1 並設定 errno。
typedef struct ntfsb_device_callbacks {
  void *context;
  uint32_t block_size;   ///< 邏輯磁區大小（位元組）
  uint64_t block_count;  ///< 磁區總數
  bool writable;         ///< 裝置是否可寫
  int64_t (*pread)(void *context, void *buf, int64_t count, int64_t offset);
  int64_t (*pwrite)(void *context, const void *buf, int64_t count, int64_t offset);
  int (*sync)(void *context);
} ntfsb_device_callbacks;

/// probe 結果
typedef struct ntfsb_probe_info {
  char label[1024];   ///< 卷標（UTF-8），無卷標時為空字串
  uint64_t serial;    ///< boot sector 內的卷序號
  uint8_t major;      ///< NTFS 版本
  uint8_t minor;
  bool dirty;         ///< 卷被標記為需要檢查（未正常卸載）
  bool hibernated;    ///< Windows 休眠中（不可安全寫入）
} ntfsb_probe_info;

/// 檔案型別
typedef enum ntfsb_type {
  NTFSB_TYPE_UNKNOWN = 0,
  NTFSB_TYPE_FILE,
  NTFSB_TYPE_DIR,
  NTFSB_TYPE_SYMLINK,
  NTFSB_TYPE_FIFO,
  NTFSB_TYPE_CHR,
  NTFSB_TYPE_BLK,
  NTFSB_TYPE_SOCK,
} ntfsb_type;

/// 檔案屬性
typedef struct ntfsb_attr {
  uint64_t mft_no;
  ntfsb_type type;
  uint32_t nlink;
  uint64_t size;        ///< 資料大小（目錄為索引大小）
  uint64_t alloc_size;  ///< 已配置大小
  bool readonly;        ///< FILE_ATTR_READONLY
  bool hidden;          ///< FILE_ATTR_HIDDEN
  bool system;          ///< FILE_ATTR_SYSTEM
  bool archive;         ///< FILE_ATTR_ARCHIVE
  bool compressed;      ///< FILE_ATTR_COMPRESSED
  bool encrypted;       ///< FILE_ATTR_ENCRYPTED
  bool sparse;          ///< FILE_ATTR_SPARSE_FILE
  bool resident;        ///< $DATA 為常駐（資料在 MFT record 內，沒有叢集）
  uint64_t init_size;   ///< initialized_size：之後到 size 之前的範圍在磁碟上是垃圾，必須讀成零
  struct timespec atime;
  struct timespec mtime;
  struct timespec ctime;
  struct timespec btime;
} ntfsb_attr;

/// 卷統計
typedef struct ntfsb_statfs_info {
  uint32_t cluster_size;
  uint32_t sector_size;
  uint64_t total_clusters;
  uint64_t free_clusters;
  uint64_t total_files;
  uint64_t free_files;
} ntfsb_statfs_info;

/// 目錄列舉回呼。回傳非 0 表示停止列舉（例如緩衝區已滿）。
/// - name：UTF-8 檔名（不含結尾 NUL 保證，請用 name_len）
/// - cookie：此項目的位置；用同一 cookie 重新呼叫 ntfsb_readdir 會再次從此項目開始
/// - next_cookie：下一個項目的位置；列舉結束時為 NTFSB_COOKIE_END
typedef int (*ntfsb_readdir_cb)(void *context,
                                const char *name,
                                size_t name_len,
                                uint64_t mft_no,
                                ntfsb_type type,
                                int64_t cookie,
                                int64_t next_cookie);

/// xattr 名稱列舉回呼。回傳非 0 表示停止。
typedef int (*ntfsb_xattr_list_cb)(void *context, const char *name, size_t name_len);

/// 目錄列舉的起始 cookie 與結束 cookie
#define NTFSB_COOKIE_START 0
#define NTFSB_COOKIE_END   INT64_MAX

/// 根目錄的 MFT 編號（FILE_root）
#define NTFSB_ROOT_MFT_NO 5

/// xattr 設定策略
typedef enum ntfsb_xattr_policy {
  NTFSB_XATTR_ALWAYS_SET = 0,
  NTFSB_XATTR_MUST_CREATE = 1,
  NTFSB_XATTR_MUST_REPLACE = 2,
} ntfsb_xattr_policy;

// MARK: - 初始化與卷層級

/// 程序啟動時呼叫一次：設定 locale 與 libntfs-3g 日誌等級。
void ntfsb_global_init(void);

/// 探測裝置是否為 NTFS。成功回傳 0 並填入 info；非 NTFS 回傳 -EINVAL。
int ntfsb_probe(const ntfsb_device_callbacks *dev, ntfsb_probe_info *info);

/// 掛載卷。read_only 為 true 時以唯讀掛載；否則以讀寫掛載，
/// 若裝置不可寫或卷處於休眠狀態則自動退回唯讀（可用 ntfsb_is_readonly 查詢）。
/// 失敗回傳 NULL 並在 out_errno 填入 errno。
ntfsb_volume *ntfsb_mount(const char *name,
                          const ntfsb_device_callbacks *dev,
                          bool read_only,
                          int *out_errno);

/// 卸載並釋放卷（會先同步所有變更）。
int ntfsb_umount(ntfsb_volume *vol);

/// 卷是否為唯讀
bool ntfsb_is_readonly(const ntfsb_volume *vol);

/// 卷標（UTF-8），生命週期同 vol
const char *ntfsb_label(const ntfsb_volume *vol);

/// 設定卷標
int ntfsb_set_label(ntfsb_volume *vol, const char *label);

/// 卷統計
int ntfsb_statfs(ntfsb_volume *vol, ntfsb_statfs_info *out);

/// 將快取變更寫回裝置
int ntfsb_sync(ntfsb_volume *vol);

// MARK: - Kernel offloaded I/O
//
// FSKit 的 KOIO 讓 kernel 拿著檔案的邏輯→實體 extent 直接對裝置 I/O，
// 橋接層只負責：判定哪些檔案能這樣做、解析 runlist、在 I/O 完成後更新大小與時間戳。
// 交出去的叢集之後不能再經由區塊快取碰它，所以 ntfsb_blockmap 會先把範圍 flush 並失效。

/// 依檔案屬性判定是否可走 KOIO：常駐、壓縮、加密、sparse 不行；
/// initialized_size < size 且 initialized_size 不在叢集邊界也不行（半滿叢集的尾巴是垃圾）。
bool ntfsb_koio_eligible(const ntfsb_attr *attr, uint32_t cluster_size);

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

// MARK: - 檔案與目錄

/// 取得屬性
int ntfsb_getattr(ntfsb_volume *vol, uint64_t mft_no, ntfsb_attr *out);

/// 在目錄中查找名稱。找不到回傳 -ENOENT。
int ntfsb_lookup(ntfsb_volume *vol,
                 uint64_t dir_mft_no,
                 const char *name,
                 uint64_t *out_mft_no,
                 ntfsb_type *out_type);

/// 取得父目錄的 MFT 編號（根目錄的父目錄為自己）
int ntfsb_parent(ntfsb_volume *vol, uint64_t mft_no, uint64_t *out_parent);

/// 列舉目錄。pos 傳入起始 cookie，回傳時更新為下一次應使用的 cookie。
/// 會包含 "." 與 ".." 兩個項目。
int ntfsb_readdir(ntfsb_volume *vol,
                  uint64_t dir_mft_no,
                  int64_t *pos,
                  ntfsb_readdir_cb callback,
                  void *context);

/// 讀取檔案資料。回傳實際讀取位元組數（可能小於 len）或負 errno。
int64_t ntfsb_read(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, void *buf, size_t len);

/// 寫入檔案資料。回傳實際寫入位元組數或負 errno。
int64_t ntfsb_write(ntfsb_volume *vol, uint64_t mft_no, int64_t offset, const void *buf, size_t len);

/// 調整檔案大小
int ntfsb_truncate(ntfsb_volume *vol, uint64_t mft_no, int64_t size);

// MARK: - 持久 handle
//
// 一般路徑（ntfsb_read / ntfsb_write）每次呼叫都要重開 inode 與 ntfs_attr，
// 並在關閉時把 MFT record 寫回磁碟。開檔期間改持有 handle 可以省掉這些成本：
// runlist 只建立一次，中繼資料延後到 ntfsb_file_flush / ntfsb_file_close 才寫回。
//
// handle 存在期間，ntfsb_read / ntfsb_write / ntfsb_truncate / ntfsb_getattr
// 會自動改用 handle 持有的 inode，因此呼叫端不必改寫既有路徑；
// 但呼叫端必須保證 handle 的建立與關閉成對，且不可跨卷使用。

/// 開啟持久 handle。只接受一般檔案：目錄回傳 EISDIR、reparse point 回傳 EOPNOTSUPP。
/// 同一個 mft_no 重複開啟會共用同一個 handle（內部引用計數）。
/// 失敗回傳 NULL 並在 out_err 填入 errno。
ntfsb_file *ntfsb_file_open(ntfsb_volume *vol, uint64_t mft_no, int *out_err);

/// 經由 handle 讀取。回傳實際讀取位元組數或負 errno。
int64_t ntfsb_file_read(ntfsb_file *file, int64_t offset, void *buf, size_t len);

/// 經由 handle 寫入。時間戳只更新在記憶體中，MFT record 不會立即寫回。
/// 回傳實際寫入位元組數或負 errno。
int64_t ntfsb_file_write(ntfsb_file *file, int64_t offset, const void *buf, size_t len);

/// 把延後的中繼資料（大小、時間戳）寫回。沒有變更時不做任何事。
int ntfsb_file_flush(ntfsb_file *file);

/// 關閉 handle（引用計數歸零時才真正關閉，並寫回延後的中繼資料）。
int ntfsb_file_close(ntfsb_file *file);

/// 把卷上所有 handle 延後的中繼資料寫回。ntfsb_sync 會自行呼叫。
int ntfsb_flush_all(ntfsb_volume *vol);

/// 是否有 handle 帶著尚未寫回的中繼資料（供呼叫端決定要不要啟動定時 flush）
bool ntfsb_has_dirty_files(const ntfsb_volume *vol);

/// 建立檔案 / 目錄 / 符號連結。type 只接受 FILE、DIR、SYMLINK、FIFO、SOCK。
/// symlink_target 只在 type == SYMLINK 時使用。
int ntfsb_create(ntfsb_volume *vol,
                 uint64_t dir_mft_no,
                 const char *name,
                 ntfsb_type type,
                 const char *symlink_target,
                 uint64_t *out_mft_no);

/// 刪除目錄中的名稱（目錄必須為空，否則回傳 -ENOTEMPTY）
int ntfsb_unlink(ntfsb_volume *vol, uint64_t dir_mft_no, const char *name, uint64_t mft_no);

/// 建立硬連結
int ntfsb_link(ntfsb_volume *vol, uint64_t mft_no, uint64_t dir_mft_no, const char *name);

/// 重新命名 / 移動。over_mft_no 非 0 時表示目標名稱已存在且應被覆蓋。
int ntfsb_rename(ntfsb_volume *vol,
                 uint64_t src_dir_mft_no,
                 const char *src_name,
                 uint64_t mft_no,
                 uint64_t dst_dir_mft_no,
                 const char *dst_name,
                 uint64_t over_mft_no);

/// 讀取符號連結目標（UTF-8，NUL 結尾）。回傳長度或負 errno。
int ntfsb_readlink(ntfsb_volume *vol, uint64_t mft_no, char *buf, size_t bufsize);

/// 設定時間戳，傳 NULL 表示不變更。
int ntfsb_set_times(ntfsb_volume *vol,
                    uint64_t mft_no,
                    const struct timespec *atime,
                    const struct timespec *mtime,
                    const struct timespec *ctime,
                    const struct timespec *btime);

/// 設定 NTFS 檔案屬性旗標。set_readonly / set_hidden 為 -1 表示不變更，0/1 表示清除/設定。
int ntfsb_set_flags(ntfsb_volume *vol, uint64_t mft_no, int set_readonly, int set_hidden);

// MARK: - 延伸屬性（對應 NTFS 具名 $DATA 串流）

/// 列舉 xattr 名稱
int ntfsb_xattr_list(ntfsb_volume *vol, uint64_t mft_no, ntfsb_xattr_list_cb callback, void *context);

/// 讀取 xattr。buf 為 NULL 時只回傳大小。回傳大小或負 errno（不存在為 -ENOATTR）。
int64_t ntfsb_xattr_get(ntfsb_volume *vol, uint64_t mft_no, const char *name, void *buf, size_t bufsize);

/// 設定 xattr
int ntfsb_xattr_set(ntfsb_volume *vol,
                    uint64_t mft_no,
                    const char *name,
                    const void *data,
                    size_t size,
                    ntfsb_xattr_policy policy);

/// 移除 xattr
int ntfsb_xattr_remove(ntfsb_volume *vol, uint64_t mft_no, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NTFS_BRIDGE_H */
