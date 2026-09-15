//
//  NTFSFairLock.m
//  NTFSFSExtension
//

#import "NTFSFairLock.h"
#import "ntfs_fairlock.h"

@implementation NTFSFairLock {
  ntfsb_fairlock _lock;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    if (ntfsb_fairlock_init(&_lock) != 0) {
      return nil;
    }
  }
  return self;
}

- (void)dealloc {
  ntfsb_fairlock_destroy(&_lock);
}

- (void)lock {
  ntfsb_fairlock_lock(&_lock);
}

- (void)unlock {
  ntfsb_fairlock_unlock(&_lock);
}

@end
