//
//  NTFSItem.h
//  NTFSFSExtension
//
//  FSItem 子類：只記錄 MFT 編號與快取的型別，不長期持有 libntfs-3g 的 inode。
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
/// 目錄內容的修改世代，作為 enumerateDirectory 的 verifier
@property (nonatomic) uint64_t generation;

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
