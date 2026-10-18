//
//  NTFSItem.h
//  NTFSFSExtension
//
//  FSItem 子類：記錄 MFT 編號與快取的型別。
//  檔案在開啟期間會持有一個橋接層 handle（ntfsb_file），關閉時釋放。
//

#import <Foundation/Foundation.h>
#import <FSKit/FSKit.h>
#import "ntfs_bridge.h"

NS_ASSUME_NONNULL_BEGIN

/// 根目錄以外的 item ID 位移，避免與 FSItemID 保留值（0、1、2）衝突
static const uint64_t NTFSItemIDOffset = 64;

@interface NTFSItem : FSItem

@property (nonatomic, readonly) uint64_t mftNo;
@property (nonatomic) ntfsb_type type;
/// 開啟計數（FSVolumeOpenCloseOperations）
@property (nonatomic) NSUInteger openCount;
/// 開啟期間持有的橋接層 handle（只有一般檔案會有；由 NTFSVolume 管理生命週期）
@property (nonatomic, nullable) ntfsb_file *file;
/// 目錄內容的修改世代，作為 enumerateDirectory 的 verifier
@property (nonatomic) uint64_t generation;
/// 對應的 MFT 記錄已被釋放（最後一個名稱被刪除）。該編號之後可能被新的檔案或目錄
/// 重複使用，這個 item 不可再拿 mftNo 去存取磁碟，只能等 FSKit 關閉並回收它。
@property (nonatomic) BOOL stale;

- (instancetype)initWithMFTNo:(uint64_t)mftNo type:(ntfsb_type)type NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// 對應的 FSItemID（根目錄固定為 FSItemIDRootDirectory）
@property (nonatomic, readonly) FSItemID itemID;

/// MFT 編號 → FSItemID
+ (FSItemID)itemIDForMFTNo:(uint64_t)mftNo;

/// ntfsb_type → FSItemType
+ (FSItemType)fsItemTypeForType:(ntfsb_type)type;

@end

NS_ASSUME_NONNULL_END
