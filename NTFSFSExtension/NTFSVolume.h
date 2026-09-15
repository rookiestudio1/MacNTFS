//
//  NTFSVolume.h
//  NTFSFSExtension
//
//  以 libntfs-3g 實作的 FSVolume。
//

#import <Foundation/Foundation.h>
#import <FSKit/FSKit.h>
#import "NTFSBlockDevice.h"
#import "ntfs_bridge.h"

NS_ASSUME_NONNULL_BEGIN

/// 本擴充在 mount -t 使用的短名稱（需與 Info.plist 的 FSShortName 一致）
extern NSString *const NTFSFSShortName;

@interface NTFSVolume : FSVolume <FSVolumeOperations,
                                  FSVolumePathConfOperations,
                                  FSVolumeOpenCloseOperations,
                                  FSVolumeReadWriteOperations,
                                  FSVolumeKernelOffloadedIOOperations,
                                  FSVolumeXattrOperations,
                                  FSVolumeRenameOperations>

- (instancetype)initWithDevice:(NTFSBlockDevice *)device
                      volumeID:(FSVolumeIdentifier *)volumeID
                    volumeName:(FSFileName *)volumeName NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithVolumeID:(FSVolumeIdentifier *)volumeID
                      volumeName:(FSFileName *)volumeName NS_UNAVAILABLE;

/// 目前是否為唯讀掛載
@property (nonatomic, readonly, getter=isReadOnly) BOOL readOnly;

/// 若已掛載則卸載（可重複呼叫）
- (void)unmountIfNeeded;

@end

NS_ASSUME_NONNULL_END
