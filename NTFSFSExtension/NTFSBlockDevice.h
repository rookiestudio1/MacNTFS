//
//  NTFSBlockDevice.h
//  NTFSFSExtension
//
//  把 FSBlockDeviceResource 包裝成 ntfs_bridge 需要的裝置回呼。
//

#import <Foundation/Foundation.h>
#import <FSKit/FSKit.h>
#import "ntfs_bridge.h"

NS_ASSUME_NONNULL_BEGIN

@interface NTFSBlockDevice : NSObject

@property (nonatomic, readonly, strong) FSBlockDeviceResource *resource;

/// 供 ntfs_bridge 使用的回呼結構（context 指向本物件，生命週期由本物件持有）
@property (nonatomic, readonly) ntfsb_device_callbacks callbacks;

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@end

NS_ASSUME_NONNULL_END
