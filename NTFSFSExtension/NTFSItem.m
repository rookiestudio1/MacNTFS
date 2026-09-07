//
//  NTFSItem.m
//  NTFSFSExtension
//

#import "NTFSItem.h"

@implementation NTFSItem

- (instancetype)initWithMFTNo:(uint64_t)mftNo type:(ntfsb_type)type {
  self = [super init];
  if (self) {
    _mftNo = mftNo;
    _type = type;
    _generation = 1;
  }
  return self;
}

- (FSItemID)itemID {
  return [NTFSItem itemIDForMFTNo:self.mftNo];
}

+ (FSItemID)itemIDForMFTNo:(uint64_t)mftNo {
  if (mftNo == NTFSB_ROOT_MFT_NO) {
    return FSItemIDRootDirectory;
  }
  return (FSItemID)(mftNo + NTFSItemIDOffset);
}

+ (FSItemType)fsItemTypeForType:(ntfsb_type)type {
  switch (type) {
    case NTFSB_TYPE_FILE: return FSItemTypeFile;
    case NTFSB_TYPE_DIR: return FSItemTypeDirectory;
    case NTFSB_TYPE_SYMLINK: return FSItemTypeSymlink;
    case NTFSB_TYPE_FIFO: return FSItemTypeFIFO;
    case NTFSB_TYPE_CHR: return FSItemTypeCharDevice;
    case NTFSB_TYPE_BLK: return FSItemTypeBlockDevice;
    case NTFSB_TYPE_SOCK: return FSItemTypeSocket;
    default: return FSItemTypeUnknown;
  }
}

- (NSString *)description {
  return [NSString stringWithFormat:@"<NTFSItem mft=%llu type=%d>", self.mftNo, (int)self.type];
}

@end
