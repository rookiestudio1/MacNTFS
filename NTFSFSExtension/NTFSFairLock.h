//
//  NTFSFairLock.h
//  NTFSFSExtension
//
//  ntfsb_fairlock 的 Objective-C 包裝，介面與 NSRecursiveLock 相同，
//  差別在於保證先進先出，不會讓中繼資料操作被持續的資料傳輸餓死。
//  原因與實測數字見 Bridge/ntfs_fairlock.h。
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface NTFSFairLock : NSObject

- (void)lock;
- (void)unlock;

@end

NS_ASSUME_NONNULL_END
