//
//  main.c
//  ntfs-bridge-test
//
//  以一般檔案（NTFS 映像檔）模擬區塊裝置，對 ntfs_bridge 做煙霧測試。
//  用法：
//    ntfs-bridge-test <image> [block_size]         正確性測試
//    ntfs-bridge-test bench <image> [block_size]   效能量測
//  所有測試通過時回傳 0，否則印出失敗項目並回傳 1。
//  映像檔可用 Scripts/make-test-image.sh 產生。
//

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include "bcache_test.h"
#include "ntfs_blockcache.h"
#include "bench.h"
#include "fairlock_test.h"
#include "file_device.h"
#include "ntfs_bridge.h"

static int failures = 0;

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

/// readdir 回呼：收集名稱
typedef struct dir_listing {
  char names[64][256];
  uint64_t mfts[64];
  int count;
  int limit;   ///< 模擬緩衝區大小：達到後拒收
  int64_t last_next;
} dir_listing;

static int collect_entry(void *ctx, const char *name, size_t len, uint64_t mft_no,
                         ntfsb_type type, int64_t cookie, int64_t next) {
  dir_listing *l = ctx;
  (void)type;
  (void)cookie;
  if (l->count >= l->limit) {
    return 1;
  }
  size_t n = len < 255 ? len : 255;
  memcpy(l->names[l->count], name, n);
  l->names[l->count][n] = 0;
  l->mfts[l->count] = mft_no;
  l->count++;
  l->last_next = next;
  return 0;
}

static bool listing_has(const dir_listing *l, const char *name) {
  for (int i = 0; i < l->count; i++) {
    if (!strcmp(l->names[i], name)) {
      return true;
    }
  }
  return false;
}

static int xattr_collect(void *ctx, const char *name, size_t len) {
  dir_listing *l = ctx;
  if (l->count >= 64) {
    return 1;
  }
  size_t n = len < 255 ? len : 255;
  memcpy(l->names[l->count], name, n);
  l->names[l->count][n] = 0;
  l->count++;
  return 0;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "用法：\n"
          "  %s <ntfs 映像檔> [block_size]         正確性測試\n"
          "  %s bench <ntfs 映像檔> [block_size]   效能量測\n",
          prog, prog);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  // bench 子命令
  if (strcmp(argv[1], "bench") == 0) {
    if (argc < 3) {
      usage(argv[0]);
      return 2;
    }
    uint32_t bs = argc > 3 ? (uint32_t)atoi(argv[3]) : 512;
    return bench_run(argv[2], bs);
  }

  // 這兩項不需要映像檔，先跑
  failures += fairlock_test_run();
  failures += bcache_test_run();

  uint32_t block_size = argc > 2 ? (uint32_t)atoi(argv[2]) : 512;
  file_device dev;
  if (file_device_open(&dev, argv[1]) < 0) {
    perror("open");
    return 2;
  }
  ntfsb_device_callbacks cb = file_device_callbacks(&dev, block_size);

  ntfsb_global_init();

  // probe
  ntfsb_probe_info info;
  int rc = ntfsb_probe(&cb, &info);
  CHECK(rc == 0, "probe 成功 (rc=%d)", rc);
  printf("  卷標=\"%s\" 序號=%016llx 版本=%d.%d dirty=%d hibernated=%d\n",
         info.label, (unsigned long long)info.serial, info.major, info.minor, info.dirty, info.hibernated);

  // mount rw
  int err = 0;
  ntfsb_volume *vol = ntfsb_mount("test", &cb, false, &err);
  CHECK(vol != NULL, "讀寫掛載 (errno=%d %s)", err, strerror(err));
  if (!vol) {
    return 1;
  }
  CHECK(!ntfsb_is_readonly(vol), "掛載為讀寫");

  ntfsb_statfs_info sf;
  rc = ntfsb_statfs(vol, &sf);
  CHECK(rc == 0 && sf.total_clusters > 0, "statfs: 叢集 %llu 可用 %llu 叢集大小 %u",
        (unsigned long long)sf.total_clusters, (unsigned long long)sf.free_clusters, sf.cluster_size);

  // root getattr
  ntfsb_attr a;
  rc = ntfsb_getattr(vol, NTFSB_ROOT_MFT_NO, &a);
  CHECK(rc == 0 && a.type == NTFSB_TYPE_DIR, "根目錄 getattr (rc=%d type=%d)", rc, a.type);

  // mkdir
  uint64_t dir_mft = 0;
  rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "測試資料夾", NTFSB_TYPE_DIR, NULL, &dir_mft);
  CHECK(rc == 0 && dir_mft >= 16, "建立目錄 (rc=%d mft=%llu)", rc, (unsigned long long)dir_mft);

  // lookup (case-insensitive)
  uint64_t found = 0;
  ntfsb_type ftype = 0;
  rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "測試資料夾", &found, &ftype);
  CHECK(rc == 0 && found == dir_mft && ftype == NTFSB_TYPE_DIR, "lookup 目錄 (rc=%d)", rc);
  rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "不存在", &found, &ftype);
  CHECK(rc == -ENOENT, "lookup 不存在的名稱回傳 ENOENT (rc=%d)", rc);

  // create file + write + read
  uint64_t file_mft = 0;
  rc = ntfsb_create(vol, dir_mft, "Hello.txt", NTFSB_TYPE_FILE, NULL, &file_mft);
  CHECK(rc == 0, "建立檔案 (rc=%d)", rc);
  const char *payload = "你好，NTFS！Hello FSKit.\n";
  size_t plen = strlen(payload);
  int64_t w = ntfsb_write(vol, file_mft, 0, payload, plen);
  CHECK(w == (int64_t)plen, "寫入 %zu 位元組 (w=%lld)", plen, (long long)w);
  // 未對齊的偏移寫入（跨 cluster）
  char big[10000];
  for (size_t i = 0; i < sizeof(big); i++) {
    big[i] = (char)('A' + (i % 26));
  }
  w = ntfsb_write(vol, file_mft, 4093, big, sizeof(big));
  CHECK(w == (int64_t)sizeof(big), "未對齊偏移寫入 (w=%lld)", (long long)w);
  rc = ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.size == 4093 + sizeof(big) && a.type == NTFSB_TYPE_FILE && a.archive,
        "檔案大小 %llu、archive 旗標", (unsigned long long)a.size);
  char rbuf[16384];
  int64_t r = ntfsb_read(vol, file_mft, 0, rbuf, sizeof(rbuf));
  CHECK(r == (int64_t)(4093 + sizeof(big)), "讀取整個檔案 (r=%lld)", (long long)r);
  CHECK(r > 0 && memcmp(rbuf, payload, plen) == 0, "開頭內容正確");
  CHECK(r > 0 && memcmp(rbuf + 4093, big, sizeof(big)) == 0, "未對齊區段內容正確");
  r = ntfsb_read(vol, file_mft, 4093 + sizeof(big) + 100, rbuf, 10);
  CHECK(r == 0, "超過 EOF 讀取回傳 0 (r=%lld)", (long long)r);

  // lookup by different case
  rc = ntfsb_lookup(vol, dir_mft, "hello.TXT", &found, &ftype);
  CHECK(rc == 0 && found == file_mft, "不分大小寫 lookup (rc=%d)", rc);

  // truncate
  rc = ntfsb_truncate(vol, file_mft, 100);
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.size == 100, "truncate 到 100 (size=%llu)", (unsigned long long)a.size);
  rc = ntfsb_truncate(vol, file_mft, 1000000);
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.size == 1000000, "truncate 放大到 1000000 (size=%llu)", (unsigned long long)a.size);
  r = ntfsb_read(vol, file_mft, 500000, rbuf, 16);
  CHECK(r == 16 && rbuf[0] == 0 && rbuf[15] == 0, "放大後的區域讀到零 (r=%lld)", (long long)r);

  // xattr
  const char *xname = "com.apple.FinderInfo";
  char xval[32];
  memset(xval, 0x5a, sizeof(xval));
  rc = ntfsb_xattr_set(vol, file_mft, xname, xval, sizeof(xval), NTFSB_XATTR_MUST_CREATE);
  CHECK(rc == 0, "xattr 建立 (rc=%d)", rc);
  rc = ntfsb_xattr_set(vol, file_mft, xname, xval, sizeof(xval), NTFSB_XATTR_MUST_CREATE);
  CHECK(rc == -EEXIST, "xattr 重複建立回傳 EEXIST (rc=%d)", rc);
  char xout[64];
  int64_t xs = ntfsb_xattr_get(vol, file_mft, xname, NULL, 0);
  CHECK(xs == (int64_t)sizeof(xval), "xattr 大小 (xs=%lld)", (long long)xs);
  xs = ntfsb_xattr_get(vol, file_mft, xname, xout, sizeof(xout));
  CHECK(xs == (int64_t)sizeof(xval) && memcmp(xout, xval, sizeof(xval)) == 0, "xattr 內容正確");
  xs = ntfsb_xattr_get(vol, file_mft, "com.apple.nothing", xout, sizeof(xout));
  CHECK(xs == -ENOATTR, "不存在的 xattr 回傳 ENOATTR (xs=%lld)", (long long)xs);
  dir_listing xl = { .limit = 64 };
  rc = ntfsb_xattr_list(vol, file_mft, xattr_collect, &xl);
  CHECK(rc == 0 && xl.count == 1 && !strcmp(xl.names[0], xname), "xattr 列表 (count=%d)", xl.count);
  rc = ntfsb_xattr_set(vol, file_mft, xname, "ab", 2, NTFSB_XATTR_MUST_REPLACE);
  xs = ntfsb_xattr_get(vol, file_mft, xname, xout, sizeof(xout));
  CHECK(rc == 0 && xs == 2 && xout[0] == 'a', "xattr 取代 (rc=%d xs=%lld)", rc, (long long)xs);
  rc = ntfsb_xattr_remove(vol, file_mft, xname);
  xs = ntfsb_xattr_get(vol, file_mft, xname, NULL, 0);
  CHECK(rc == 0 && xs == -ENOATTR, "xattr 移除 (rc=%d xs=%lld)", rc, (long long)xs);
  // 檔案大小不受 xattr 影響
  rc = ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.size == 1000000, "xattr 操作後主資料大小不變 (size=%llu)", (unsigned long long)a.size);

  // symlink
  uint64_t link_mft = 0;
  rc = ntfsb_create(vol, dir_mft, "link", NTFSB_TYPE_SYMLINK, "Hello.txt", &link_mft);
  CHECK(rc == 0, "建立符號連結 (rc=%d)", rc);
  char target[512];
  rc = ntfsb_readlink(vol, link_mft, target, sizeof(target));
  CHECK(rc > 0 && !strcmp(target, "Hello.txt"), "readlink (rc=%d target=%s)", rc, target);
  rc = ntfsb_getattr(vol, link_mft, &a);
  CHECK(rc == 0 && a.type == NTFSB_TYPE_SYMLINK, "符號連結 getattr 型別 (type=%d)", a.type);

  // hard link
  rc = ntfsb_link(vol, file_mft, NTFSB_ROOT_MFT_NO, "hardlink.txt");
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.nlink == 2, "硬連結 nlink=2 (nlink=%u)", a.nlink);

  // forbidden name
  uint64_t bad = 0;
  rc = ntfsb_create(vol, dir_mft, "bad?name", NTFSB_TYPE_FILE, NULL, &bad);
  CHECK(rc == -EINVAL, "Windows 不允許的檔名回傳 EINVAL (rc=%d)", rc);

  // set times / flags
  struct timespec ts = { .tv_sec = 1700000000, .tv_nsec = 123456700 };
  rc = ntfsb_set_times(vol, file_mft, NULL, &ts, NULL, NULL);
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.mtime.tv_sec == ts.tv_sec && a.mtime.tv_nsec == ts.tv_nsec,
        "設定 mtime (%ld.%09ld)", (long)a.mtime.tv_sec, a.mtime.tv_nsec);
  rc = ntfsb_set_flags(vol, file_mft, 1, 1);
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && a.readonly && a.hidden, "設定 readonly/hidden 旗標");
  rc = ntfsb_set_flags(vol, file_mft, 0, 0);
  rc |= ntfsb_getattr(vol, file_mft, &a);
  CHECK(rc == 0 && !a.readonly && !a.hidden, "清除 readonly/hidden 旗標");

  // readdir 完整列舉
  dir_listing l = { .limit = 64 };
  int64_t pos = NTFSB_COOKIE_START;
  rc = ntfsb_readdir(vol, dir_mft, &pos, collect_entry, &l);
  CHECK(rc == 0 && pos == NTFSB_COOKIE_END, "readdir 完整列舉 (rc=%d pos=%lld count=%d)", rc, (long long)pos, l.count);
  CHECK(listing_has(&l, ".") && listing_has(&l, "..") && listing_has(&l, "Hello.txt") && listing_has(&l, "link"),
        "readdir 包含 . .. Hello.txt link");

  // readdir 分段列舉（每次只收一筆）
  dir_listing all = { .limit = 64 };
  pos = NTFSB_COOKIE_START;
  int rounds = 0;
  while (pos != NTFSB_COOKIE_END && rounds < 100) {
    dir_listing one = { .limit = 1 };
    rc = ntfsb_readdir(vol, dir_mft, &pos, collect_entry, &one);
    if (rc) {
      break;
    }
    if (one.count == 1) {
      strcpy(all.names[all.count], one.names[0]);
      all.count++;
    }
    rounds++;
  }
  CHECK(rc == 0 && all.count == l.count, "readdir 分段列舉筆數一致 (%d vs %d, rounds=%d)", all.count, l.count, rounds);
  for (int i = 0; i < l.count; i++) {
    CHECK(listing_has(&all, l.names[i]), "分段列舉包含 %s", l.names[i]);
  }

  // rename within dir
  rc = ntfsb_rename(vol, dir_mft, "Hello.txt", file_mft, dir_mft, "Renamed.txt", 0);
  CHECK(rc == 0, "重新命名 (rc=%d)", rc);
  rc = ntfsb_lookup(vol, dir_mft, "Renamed.txt", &found, NULL);
  CHECK(rc == 0 && found == file_mft, "重新命名後 lookup 新名稱");
  rc = ntfsb_lookup(vol, dir_mft, "Hello.txt", &found, NULL);
  CHECK(rc == -ENOENT, "重新命名後舊名稱消失 (rc=%d)", rc);

  // move to root over existing (hardlink.txt is the same inode → no-op)
  rc = ntfsb_rename(vol, dir_mft, "Renamed.txt", file_mft, NTFSB_ROOT_MFT_NO, "hardlink.txt", file_mft);
  CHECK(rc == 0, "覆蓋同一 inode 視為無動作 (rc=%d)", rc);

  // rename over a different existing file
  uint64_t other = 0;
  rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "other.txt", NTFSB_TYPE_FILE, NULL, &other);
  rc |= ntfsb_rename(vol, dir_mft, "Renamed.txt", file_mft, NTFSB_ROOT_MFT_NO, "other.txt", other);
  CHECK(rc == 0, "覆蓋既有檔案的搬移 (rc=%d)", rc);
  rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "other.txt", &found, NULL);
  CHECK(rc == 0 && found == file_mft, "覆蓋後名稱指向來源 inode");

  // rmdir non-empty
  rc = ntfsb_unlink(vol, NTFSB_ROOT_MFT_NO, "測試資料夾", dir_mft);
  CHECK(rc == -ENOTEMPTY, "刪除非空目錄回傳 ENOTEMPTY (rc=%d)", rc);
  rc = ntfsb_unlink(vol, dir_mft, "link", link_mft);
  CHECK(rc == 0, "刪除符號連結 (rc=%d)", rc);
  rc = ntfsb_unlink(vol, NTFSB_ROOT_MFT_NO, "測試資料夾", dir_mft);
  CHECK(rc == 0, "刪除空目錄 (rc=%d)", rc);
  rc = ntfsb_unlink(vol, NTFSB_ROOT_MFT_NO, "hardlink.txt", file_mft);
  rc |= ntfsb_unlink(vol, NTFSB_ROOT_MFT_NO, "other.txt", file_mft);
  CHECK(rc == 0, "刪除所有硬連結 (rc=%d)", rc);
  rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "other.txt", &found, NULL);
  CHECK(rc == -ENOENT, "刪除後檔案消失");

  // ---- 未對齊讀取不應放大 ----
  // ntfsb_dev_pread 的未對齊路徑會透過 bounce buffer 讀取；它只該讀涵蓋請求所需
  // 的對齊範圍，不是每次都讀滿整個 buffer。
  {
    uint64_t umft = 0;
    rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "unaligned.bin", NTFSB_TYPE_FILE, NULL, &umft);
    CHECK(rc == 0 && umft >= 16, "建立 unaligned.bin (rc=%d mft=%llu)",
          rc, (unsigned long long)umft);
    if (rc != 0 || umft < 16) {
      goto skip_unaligned;  // mft_no 0 是 $MFT 本身，絕不能拿來寫
    }
    char big[8192];
    memset(big, 'U', sizeof(big));
    // 要夠大才會是 non-resident，讀取才會真的碰到裝置
    int64_t w = ntfsb_write(vol, umft, 0, big, sizeof(big));
    CHECK(w == (int64_t)sizeof(big), "寫入 8 KB (w=%lld)", (long long)w);
    rc = ntfsb_sync(vol);
    CHECK(rc == 0, "sync 後再讀 (rc=%d)", rc);

    char small[100];
    file_device_reset_stats(&dev);
    int64_t r = ntfsb_read(vol, umft, 0, small, sizeof(small));
    uint64_t got_bytes = dev.stats.read_bytes;
    CHECK(r == (int64_t)sizeof(small) && small[0] == 'U', "讀取 100 位元組 (r=%lld)", (long long)r);
    // 100 位元組的請求最多只該讀一兩個磁區；放寬到 8 KB 仍能抓出 256 KB 的放大
    CHECK(got_bytes <= 8192,
          "未對齊小讀取不放大：裝置讀取 %llu 位元組（應 <= 8192）",
          (unsigned long long)got_bytes);
  skip_unaligned:;
  }

  // ---- 大於 bounce buffer 的未對齊讀取（走多輪迴圈）----
  {
    uint64_t lmft = 0;
    rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "bigunaligned.bin", NTFSB_TYPE_FILE, NULL, &lmft);
    CHECK(rc == 0 && lmft >= 16, "建立 bigunaligned.bin (rc=%d mft=%llu)",
          rc, (unsigned long long)lmft);
    const size_t big_size = 1024 * 1024;
    uint8_t *src = (rc == 0 && lmft >= 16) ? malloc(big_size) : NULL;
    uint8_t *dst = src ? malloc(big_size) : NULL;
    if (src && dst) {
      for (size_t i = 0; i < big_size; i++) {
        src[i] = (uint8_t)(i * 31 + 7);
      }
      int64_t w = ntfsb_write(vol, lmft, 0, src, big_size);
      CHECK(w == (int64_t)big_size, "寫入 1 MB (w=%lld)", (long long)w);
      rc = ntfsb_sync(vol);

      // 從未對齊位移讀取 300 KB：超過 256 KB 的 bounce buffer，會走多輪迴圈
      const size_t want = 300 * 1024;
      file_device_reset_stats(&dev);
      int64_t r = ntfsb_read(vol, lmft, 1, dst, want);
      uint64_t got_bytes = dev.stats.read_bytes;
      CHECK(r == (int64_t)want, "未對齊讀取 300 KB (r=%lld)", (long long)r);
      CHECK(r == (int64_t)want && memcmp(dst, src + 1, want) == 0,
            "未對齊大讀取內容正確");
      // 超過 bypass 門檻的部分直讀，低於門檻的尾段會走區塊快取並對齊到完整的行，
      // 所以容許多讀兩行；這仍足以抓出「整個 bounce buffer 都讀進來」那種放大。
      const size_t limit = want + 2 * NTFSB_BCACHE_LINE;
      CHECK(got_bytes <= limit,
            "未對齊大讀取不放大：裝置讀取 %llu 位元組（應 <= %zu）",
            (unsigned long long)got_bytes, limit);
    }
    free(src);
    free(dst);
  }

  // ---- 持久 handle ----
  {
    uint64_t hmft = 0;
    rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "handle.bin", NTFSB_TYPE_FILE, NULL, &hmft);
    CHECK(rc == 0 && hmft >= 16, "建立 handle.bin (rc=%d mft=%llu)",
          rc, (unsigned long long)hmft);
    if (rc != 0 || hmft < 16) {
      goto skip_handle;
    }

    int herr = 0;
    ntfsb_file *hf = ntfsb_file_open(vol, hmft, &herr);
    CHECK(hf != NULL, "開啟持久 handle (errno=%d)", herr);

    // 分多次寫入，中繼資料應延後到 flush / close 才寫回
    char chunk[4096];
    memset(chunk, 'H', sizeof(chunk));
    int64_t total = 0;
    for (int i = 0; i < 8; i++) {
      int64_t w = ntfsb_file_write(hf, total, chunk, sizeof(chunk));
      if (w != (int64_t)sizeof(chunk)) {
        break;
      }
      total += w;
    }
    CHECK(total == 8 * (int64_t)sizeof(chunk), "handle 分次寫入 %lld 位元組", (long long)total);

    // handle 開著時 getattr 必須看到即時大小，否則 Finder 會看到舊值
    ntfsb_attr ha;
    rc = ntfsb_getattr(vol, hmft, &ha);
    CHECK(rc == 0 && ha.size == (uint64_t)total,
          "handle 開啟時 getattr 看到即時大小 (rc=%d size=%llu 應為 %lld)",
          rc, (unsigned long long)ha.size, (long long)total);

    // 一般讀取路徑應自動改用 handle，不能另開一份 inode
    char verify[4096];
    int64_t r = ntfsb_read(vol, hmft, 4096, verify, sizeof(verify));
    CHECK(r == (int64_t)sizeof(verify) && verify[0] == 'H' && verify[4095] == 'H',
          "handle 開啟時 ntfsb_read 走 handle 路徑 (r=%lld)", (long long)r);

    // truncate 也必須沿用 handle 的 ntfs_attr
    rc = ntfsb_truncate(vol, hmft, 5000);
    CHECK(rc == 0, "handle 開啟時 truncate (rc=%d)", rc);
    rc = ntfsb_getattr(vol, hmft, &ha);
    CHECK(rc == 0 && ha.size == 5000, "truncate 後大小為 5000（實得 %llu）",
          (unsigned long long)ha.size);

    // 引用計數：重複開啟共用同一個 handle
    ntfsb_file *hf2 = ntfsb_file_open(vol, hmft, &herr);
    CHECK(hf2 == hf, "重複開啟共用同一個 handle");
    rc = ntfsb_file_close(hf2);
    CHECK(rc == 0, "釋放第二個引用 (rc=%d)", rc);
    // 第一個引用還在，寫入仍應成功
    int64_t w2 = ntfsb_file_write(hf, 0, "Z", 1);
    CHECK(w2 == 1, "釋放第二個引用後 handle 仍可用 (w=%lld)", (long long)w2);

    rc = ntfsb_file_close(hf);
    CHECK(rc == 0, "關閉持久 handle (rc=%d)", rc);

    // 關閉後回到一般路徑，資料與大小都要正確
    rc = ntfsb_getattr(vol, hmft, &ha);
    CHECK(rc == 0 && ha.size == 5000, "關閉後大小仍為 5000（實得 %llu）",
          (unsigned long long)ha.size);
    r = ntfsb_read(vol, hmft, 0, verify, 1);
    CHECK(r == 1 && verify[0] == 'Z', "關閉後內容正確 (r=%lld)", (long long)r);
  skip_handle:;
  }

  // 留下一個檔案供外部工具驗證
  uint64_t keep = 0;
  rc = ntfsb_create(vol, NTFSB_ROOT_MFT_NO, "keep.txt", NTFSB_TYPE_FILE, NULL, &keep);
  rc |= (int)(ntfsb_write(vol, keep, 0, "kept\n", 5) != 5);
  CHECK(rc == 0, "建立保留檔案 keep.txt");

  rc = ntfsb_sync(vol);
  CHECK(rc == 0, "sync (rc=%d)", rc);

  // set label
  rc = ntfsb_set_label(vol, "BridgeTest");
  CHECK(rc == 0 && !strcmp(ntfsb_label(vol), "BridgeTest"), "設定卷標 (rc=%d)", rc);

  rc = ntfsb_umount(vol);
  CHECK(rc == 0, "卸載 (rc=%d)", rc);

  // 重新以唯讀掛載驗證持久化
  vol = ntfsb_mount("test", &cb, true, &err);
  CHECK(vol != NULL && ntfsb_is_readonly(vol), "唯讀重新掛載");
  if (vol) {
    CHECK(!strcmp(ntfsb_label(vol), "BridgeTest"), "卷標持久化：%s", ntfsb_label(vol));

    // handle 的延後中繼資料必須在 close 時寫回，重新掛載後大小與內容都要正確
    uint64_t hmft2 = 0;
    rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "handle.bin", &hmft2, NULL);
    CHECK(rc == 0, "handle.bin 持久化 (rc=%d)", rc);
    if (rc == 0) {
      ntfsb_attr ha2;
      rc = ntfsb_getattr(vol, hmft2, &ha2);
      CHECK(rc == 0 && ha2.size == 5000,
            "handle.bin 大小持久化為 5000（實得 %llu）", (unsigned long long)ha2.size);
      char vb[2];
      int64_t r2 = ntfsb_read(vol, hmft2, 0, vb, 1);
      CHECK(r2 == 1 && vb[0] == 'Z', "handle.bin 內容持久化 (r=%lld)", (long long)r2);
    }
    rc = ntfsb_lookup(vol, NTFSB_ROOT_MFT_NO, "keep.txt", &found, NULL);
    CHECK(rc == 0, "keep.txt 持久化");
    r = ntfsb_read(vol, found, 0, rbuf, 5);
    CHECK(r == 5 && memcmp(rbuf, "kept\n", 5) == 0, "keep.txt 內容持久化");
    w = ntfsb_write(vol, found, 0, "x", 1);
    CHECK(w == -EROFS, "唯讀卷寫入回傳 EROFS (w=%lld)", (long long)w);
    ntfsb_umount(vol);
  }

  file_device_close(&dev);
  printf("\n%s：%d 項失敗\n", failures ? "測試失敗" : "全部通過", failures);
  return failures ? 1 : 0;
}
