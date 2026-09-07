//
//  NTFSFileSystem.h
//  NTFSFSExtension
//
//  FSUnaryFileSystem 實作：探測、載入、卸載與 fsck。
//

#import <Foundation/Foundation.h>
#import <FSKit/FSKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface NTFSFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations,
                                               FSManageableResourceMaintenanceOperations>
@end

NS_ASSUME_NONNULL_END
