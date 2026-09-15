# libntfs-3g 本地修正

`Scripts/build-ntfs3g.sh` 解壓原始碼後會依檔名順序套用這裡的 patch，
任何一個套用失敗就中止建置（寧可停下來，也不要悄悄產出未修正的函式庫）。

## 0001-ntfs_readdir-resume-bitmap-bit-offset.patch

`ntfs_readdir()` 從 cookie 續傳進入 `$INDEX_ALLOCATION` 時，會讀一段索引
bitmap 到緩衝區，起點是位元組 `bmp_pos >> 3`；但接著把 `bmp_buf_pos`
設為 0，等於去檢查該位元組第 0 個位元，也就是區塊 `bmp_pos & ~7` 的狀態，
而不是區塊 `bmp_pos`。

續傳到非 8 對齊的區塊時就會錯位：若那個較早的區塊正在使用中，迴圈會立刻
結束，然後去讀區塊 `bmp_pos`——即使它已經超出索引配置的範圍，於是
`ntfs_attr_mst_pread` 讀過檔尾、回報 `Failed to read index block` 與 EIO。

一次列舉完不會踩到，因為從 `bmp_pos == 0` 開始，兩者始終同步；只有分頁
列舉（FSKit 的封包填滿後用 cookie 續傳）才會出現。症狀是大目錄 `ls` 到
一半回報 `fts_read: Input/output error`。

修正是把初始值改成 `bmp_pos & 7`。迴圈內重填緩衝區那一處維持 0 是正確的，
因為重填必定發生在 `bmp_pos` 8 對齊的時候。
