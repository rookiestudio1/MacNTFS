//
//  NTFSVolume.m
//  NTFSFSExtension
//

#import "NTFSVolume.h"
#import "NTFSItem.h"
#import <os/log.h>
#import <sys/stat.h>
#import <sys/fcntl.h>
#import <errno.h>

NSString *const NTFSFSShortName = @"ntfs3g";

/// 未指定 -u/-g 時使用的 uid/gid（99 = unknown，等同 Finder「忽略此卷的擁有權」）
static const uint32_t NTFSDefaultUID = 99;
static const uint32_t NTFSDefaultGID = 99;
/// 未指定 -m 時的基礎權限
static const uint32_t NTFSDefaultMode = 0777;

static os_log_t NTFSVolumeLog(void) {
  static os_log_t log;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    log = os_log_create("com.rookiestudio.MacNTFS", "volume");
  });
  return log;
}

/// 負 errno → NSError
static NSError *NTFSError(int negErrno) {
  return fs_errorForPOSIXError(negErrno < 0 ? -negErrno : negErrno);
}

/// FSFileName → UTF-8 C 字串（非合法 UTF-8 時回傳 nil）
static NSString *_Nullable NTFSStringFromName(FSFileName *name) {
  NSString *s = name.string;
  if (!s) {
    s = [[NSString alloc] initWithData:name.data encoding:NSUTF8StringEncoding];
  }
  return s;
}

/// 從 struct timespec 建立 FSKit 用的時間
static inline struct timespec NTFSTimespec(struct timespec ts) {
  return ts;
}

@interface NTFSVolume ()
@property (nonatomic, strong) NTFSBlockDevice *device;
@property (nonatomic, strong) NSRecursiveLock *lock;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NTFSItem *> *items;
@property (nonatomic, strong, nullable) NTFSItem *rootItem;
@property (nonatomic) uint32_t uid;
@property (nonatomic) uint32_t gid;
@property (nonatomic) uint32_t baseMode;
@property (nonatomic) BOOL wantReadOnly;
@end

@implementation NTFSVolume {
  ntfsb_volume *_vol;
  NSUInteger _requestedMountOptions;
}

// FSMountOptions 只在 macOS 26.4+ 提供，因此手動實作存取子並標示可用性
- (FSMountOptions)requestedMountOptions API_AVAILABLE(macos(26.4)) {
  return (FSMountOptions)_requestedMountOptions;
}

- (void)setRequestedMountOptions:(FSMountOptions)requestedMountOptions API_AVAILABLE(macos(26.4)) {
  _requestedMountOptions = (NSUInteger)requestedMountOptions;
}

- (instancetype)initWithDevice:(NTFSBlockDevice *)device
                      volumeID:(FSVolumeIdentifier *)volumeID
                    volumeName:(FSFileName *)volumeName {
  self = [super initWithVolumeID:volumeID volumeName:volumeName];
  if (self) {
    _device = device;
    _lock = [[NSRecursiveLock alloc] init];
    _items = [NSMutableDictionary dictionary];
    _uid = NTFSDefaultUID;
    _gid = NTFSDefaultGID;
    _baseMode = NTFSDefaultMode;
  }
  return self;
}

- (void)dealloc {
  [self unmountIfNeeded];
}

- (BOOL)isReadOnly {
  [self.lock lock];
  BOOL ro = !_vol || ntfsb_is_readonly(_vol);
  [self.lock unlock];
  return ro;
}

- (void)unmountIfNeeded {
  [self.lock lock];
  if (_vol) {
    os_log_info(NTFSVolumeLog(), "卸載 %{public}@", self.device.resource.BSDName);
    int rc = ntfsb_umount(_vol);
    if (rc) {
      os_log_error(NTFSVolumeLog(), "ntfsb_umount 失敗: %d", rc);
    }
    _vol = NULL;
  }
  [self.items removeAllObjects];
  self.rootItem = nil;
  [self.lock unlock];
}

// MARK: - Item 快取

/// 取得或建立對應 MFT 編號的 item（需持有鎖）
- (NTFSItem *)itemForMFTNo:(uint64_t)mftNo type:(ntfsb_type)type {
  NSNumber *key = @(mftNo);
  NTFSItem *item = self.items[key];
  if (!item) {
    item = [[NTFSItem alloc] initWithMFTNo:mftNo type:type];
    self.items[key] = item;
  } else if (type != NTFSB_TYPE_UNKNOWN) {
    item.type = type;
  }
  return item;
}

- (NTFSItem *_Nullable)ntfsItem:(FSItem *)item {
  if ([item isKindOfClass:NTFSItem.class]) {
    return (NTFSItem *)item;
  }
  return nil;
}

// MARK: - 屬性轉換

/// 由 ntfsb_attr 建立 FSItemAttributes
- (FSItemAttributes *)attributesFromBridge:(const ntfsb_attr *)a
                                 requested:(FSItemGetAttributesRequest *_Nullable)req {
  FSItemAttributes *attrs = [[FSItemAttributes alloc] init];
  [attrs invalidateAllProperties];
  FSItemType type = [NTFSItem fsItemTypeForType:a->type];
  attrs.type = type;
  uint32_t mode = self.baseMode & 0777;
  if (a->readonly) {
    mode &= ~0222;
  }
  switch (a->type) {
    case NTFSB_TYPE_DIR: mode |= S_IFDIR; break;
    case NTFSB_TYPE_SYMLINK: mode = S_IFLNK | 0777; break;
    case NTFSB_TYPE_FIFO: mode |= S_IFIFO; break;
    case NTFSB_TYPE_CHR: mode |= S_IFCHR; break;
    case NTFSB_TYPE_BLK: mode |= S_IFBLK; break;
    case NTFSB_TYPE_SOCK: mode |= S_IFSOCK; break;
    default: mode |= S_IFREG; break;
  }
  attrs.mode = mode;
  attrs.linkCount = a->nlink ? a->nlink : 1;
  attrs.uid = self.uid;
  attrs.gid = self.gid;
  attrs.flags = a->hidden ? UF_HIDDEN : 0;
  attrs.size = a->size;
  attrs.allocSize = a->alloc_size;
  attrs.fileID = [NTFSItem itemIDForMFTNo:a->mft_no];
  if (!req || [req isAttributeWanted:FSItemAttributeParentID]) {
    uint64_t parent = 0;
    if (ntfsb_parent(_vol, a->mft_no, &parent) == 0) {
      attrs.parentID = [NTFSItem itemIDForMFTNo:parent];
    }
  }
  attrs.accessTime = NTFSTimespec(a->atime);
  attrs.modifyTime = NTFSTimespec(a->mtime);
  attrs.changeTime = NTFSTimespec(a->ctime);
  attrs.birthTime = NTFSTimespec(a->btime);
  attrs.addedTime = NTFSTimespec(a->btime);
  attrs.backupTime = (struct timespec){0, 0};
  attrs.supportsLimitedXAttrs = NO;
  attrs.inhibitKernelOffloadedIO = YES;
  return attrs;
}

/// 取得 item 的最新屬性（需持有鎖）
- (FSItemAttributes *_Nullable)currentAttributesOf:(NTFSItem *)item
                                         requested:(FSItemGetAttributesRequest *_Nullable)req
                                             error:(NSError **)error {
  ntfsb_attr a;
  int rc = ntfsb_getattr(_vol, item.mftNo, &a);
  if (rc) {
    if (error) {
      *error = NTFSError(rc);
    }
    return nil;
  }
  item.type = a.type;
  return [self attributesFromBridge:&a requested:req];
}

// MARK: - 掛載選項

/// 解析 activate 選項：-o rdonly,... / -u uid / -g gid / -m mode
- (void)applyTaskOptions:(FSTaskOptions *)options {
  NSArray<NSString *> *args = options.taskOptions;
  for (NSUInteger i = 0; i < args.count; i++) {
    NSString *arg = args[i];
    NSString *value = nil;
    NSString *key = nil;
    if ([arg hasPrefix:@"-"] && arg.length == 2 && i + 1 < args.count) {
      key = arg;
      value = args[++i];
    } else if ([arg hasPrefix:@"-"] && arg.length > 2) {
      key = [arg substringToIndex:2];
      value = [arg substringFromIndex:2];
    } else {
      continue;
    }
    if ([key isEqualToString:@"-o"]) {
      for (NSString *opt in [value componentsSeparatedByString:@","]) {
        NSString *o = [opt stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([o isEqualToString:@"rdonly"] || [o isEqualToString:@"ro"]) {
          self.wantReadOnly = YES;
        } else if ([o hasPrefix:@"uid="]) {
          self.uid = (uint32_t)[o substringFromIndex:4].integerValue;
        } else if ([o hasPrefix:@"gid="]) {
          self.gid = (uint32_t)[o substringFromIndex:4].integerValue;
        } else if ([o hasPrefix:@"umask="]) {
          self.baseMode = 0777 & ~(uint32_t)strtoul([o substringFromIndex:6].UTF8String, NULL, 8);
        }
      }
    } else if ([key isEqualToString:@"-u"]) {
      self.uid = (uint32_t)value.integerValue;
    } else if ([key isEqualToString:@"-g"]) {
      self.gid = (uint32_t)value.integerValue;
    } else if ([key isEqualToString:@"-m"]) {
      self.baseMode = (uint32_t)strtoul(value.UTF8String, NULL, 8) & 0777;
    }
  }
  if (@available(macOS 26.4, *)) {
    if (_requestedMountOptions & FSMountOptionsReadOnly) {
      self.wantReadOnly = YES;
    }
  }
}

// MARK: - FSVolumeOperations：能力與統計

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities {
  FSVolumeSupportedCapabilities *caps = [[FSVolumeSupportedCapabilities alloc] init];
  caps.supportsPersistentObjectIDs = YES;
  caps.supportsSymbolicLinks = YES;
  caps.supportsHardLinks = YES;
  caps.supportsJournal = NO;
  caps.supportsActiveJournal = NO;
  caps.doesNotSupportRootTimes = NO;
  caps.supportsSparseFiles = YES;
  caps.supportsZeroRuns = NO;
  caps.supportsFastStatFS = YES;
  caps.supports2TBFiles = YES;
  caps.supportsOpenDenyModes = NO;
  caps.supportsHiddenFiles = YES;
  caps.doesNotSupportVolumeSizes = NO;
  caps.supports64BitObjectIDs = YES;
  caps.supportsDocumentID = NO;
  caps.doesNotSupportImmutableFiles = YES;
  caps.doesNotSupportSettingFilePermissions = YES;
  caps.supportsSharedSpace = NO;
  caps.supportsVolumeGroups = NO;
  caps.caseFormat = FSVolumeCaseFormatInsensitiveCasePreserving;
  return caps;
}

- (FSStatFSResult *)volumeStatistics {
  FSStatFSResult *result = [[FSStatFSResult alloc] initWithFileSystemTypeName:NTFSFSShortName];
  [self.lock lock];
  ntfsb_statfs_info sf;
  if (_vol && ntfsb_statfs(_vol, &sf) == 0) {
    uint64_t bs = sf.cluster_size ? sf.cluster_size : 4096;
    result.blockSize = (NSInteger)bs;
    result.ioSize = 1024 * 1024;
    result.totalBlocks = sf.total_clusters;
    result.freeBlocks = sf.free_clusters;
    result.availableBlocks = sf.free_clusters;
    result.usedBlocks = sf.total_clusters - sf.free_clusters;
    result.totalBytes = sf.total_clusters * bs;
    result.freeBytes = sf.free_clusters * bs;
    result.availableBytes = sf.free_clusters * bs;
    result.usedBytes = (sf.total_clusters - sf.free_clusters) * bs;
    result.totalFiles = sf.total_files;
    result.freeFiles = sf.free_files;
  }
  [self.lock unlock];
  result.fileSystemSubType = 0;
  return result;
}

// MARK: - FSVolumePathConfOperations

- (NSInteger)maximumLinkCount {
  return 1023;
}

- (NSInteger)maximumNameLength {
  return 255;
}

- (BOOL)restrictsOwnershipChanges {
  return YES;
}

- (BOOL)truncatesLongNames {
  return NO;
}

- (NSInteger)maximumXattrSize {
  return 64 * 1024 * 1024;
}

- (uint64_t)maximumFileSize {
  return UINT64_MAX;
}

// MARK: - FSVolumeOperations：生命週期

- (void)activateWithOptions:(FSTaskOptions *)options
               replyHandler:(void (^)(FSItem *_Nullable, NSError *_Nullable))reply {
  [self.lock lock];
  if (_vol) {
    [self.lock unlock];
    reply(self.rootItem, nil);
    return;
  }
  [self applyTaskOptions:options];
  ntfsb_device_callbacks cb = self.device.callbacks;
  int err = 0;
  os_log_info(NTFSVolumeLog(), "掛載 %{public}@ readOnly=%d", self.device.resource.BSDName, self.wantReadOnly);
  _vol = ntfsb_mount(self.device.resource.BSDName.UTF8String, &cb, self.wantReadOnly, &err);
  if (!_vol) {
    [self.lock unlock];
    os_log_error(NTFSVolumeLog(), "掛載失敗 errno=%d", err);
    reply(nil, fs_errorForPOSIXError(err ? err : EIO));
    return;
  }
  if (ntfsb_is_readonly(_vol) && !self.wantReadOnly) {
    os_log_info(NTFSVolumeLog(), "卷已退回唯讀掛載（休眠或裝置唯讀）");
  }
  const char *label = ntfsb_label(_vol);
  if (label && label[0]) {
    self.name = [FSFileName nameWithString:[NSString stringWithUTF8String:label]];
  }
  self.rootItem = [self itemForMFTNo:NTFSB_ROOT_MFT_NO type:NTFSB_TYPE_DIR];
  NTFSItem *root = self.rootItem;
  [self.lock unlock];
  reply(root, nil);
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options
                 replyHandler:(void (^)(NSError *_Nullable))reply {
  [self unmountIfNeeded];
  reply(nil);
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *_Nullable))reply {
  // 實際掛載已在 activate 完成
  reply(nil);
}

- (void)unmountWithReplyHandler:(void (^)(void))reply {
  [self.lock lock];
  if (_vol) {
    ntfsb_sync(_vol);
  }
  [self.lock unlock];
  reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *_Nullable))reply {
  [self.lock lock];
  int rc = _vol ? ntfsb_sync(_vol) : 0;
  [self.lock unlock];
  reply(rc ? NTFSError(rc) : nil);
}

// MARK: - FSVolumeOperations：屬性

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
               ofItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  NSError *error = nil;
  FSItemAttributes *attrs = _vol ? [self currentAttributesOf:ni requested:desiredAttributes error:&error]
                                 : nil;
  [self.lock unlock];
  reply(attrs, attrs ? nil : (error ?: NTFSError(ENXIO)));
}

- (void)setAttributes:(FSItemSetAttributesRequest *)newAttributes
               onItem:(FSItem *)item
         replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(nil, NTFSError(ENXIO));
    return;
  }
  int rc = 0;
  FSItemAttribute consumed = 0;
  // 大小
  if ([newAttributes isValid:FSItemAttributeSize]) {
    rc = ntfsb_truncate(_vol, ni.mftNo, (int64_t)newAttributes.size);
    if (rc) {
      goto done;
    }
    consumed |= FSItemAttributeSize;
  }
  // 時間
  {
    struct timespec atime, mtime, ctime, btime;
    const struct timespec *pa = NULL, *pm = NULL, *pc = NULL, *pb = NULL;
    if ([newAttributes isValid:FSItemAttributeAccessTime]) {
      atime = newAttributes.accessTime;
      pa = &atime;
      consumed |= FSItemAttributeAccessTime;
    }
    if ([newAttributes isValid:FSItemAttributeModifyTime]) {
      mtime = newAttributes.modifyTime;
      pm = &mtime;
      consumed |= FSItemAttributeModifyTime;
    }
    if ([newAttributes isValid:FSItemAttributeChangeTime]) {
      ctime = newAttributes.changeTime;
      pc = &ctime;
      consumed |= FSItemAttributeChangeTime;
    }
    if ([newAttributes isValid:FSItemAttributeBirthTime]) {
      btime = newAttributes.birthTime;
      pb = &btime;
      consumed |= FSItemAttributeBirthTime;
    }
    if (pa || pm || pc || pb) {
      rc = ntfsb_set_times(_vol, ni.mftNo, pa, pm, pc, pb);
      if (rc) {
        goto done;
      }
    }
  }
  // 權限：把擁有者寫入位元對應到 NTFS 唯讀旗標
  {
    int setReadOnly = -1;
    int setHidden = -1;
    if ([newAttributes isValid:FSItemAttributeMode]) {
      setReadOnly = (newAttributes.mode & 0200) ? 0 : 1;
      consumed |= FSItemAttributeMode;
    }
    if ([newAttributes isValid:FSItemAttributeFlags]) {
      setHidden = (newAttributes.flags & UF_HIDDEN) ? 1 : 0;
      consumed |= FSItemAttributeFlags;
    }
    if (setReadOnly >= 0 || setHidden >= 0) {
      rc = ntfsb_set_flags(_vol, ni.mftNo, setReadOnly, setHidden);
      if (rc) {
        goto done;
      }
    }
  }
  // 擁有者：NTFS 未做使用者對應，直接視為已處理
  if ([newAttributes isValid:FSItemAttributeUID]) {
    consumed |= FSItemAttributeUID;
  }
  if ([newAttributes isValid:FSItemAttributeGID]) {
    consumed |= FSItemAttributeGID;
  }
done:
  newAttributes.consumedAttributes = consumed;
  if (rc) {
    [self.lock unlock];
    reply(nil, NTFSError(rc));
    return;
  }
  NSError *error = nil;
  FSItemAttributes *attrs = [self currentAttributesOf:ni requested:nil error:&error];
  [self.lock unlock];
  reply(attrs, attrs ? nil : error);
}

// MARK: - FSVolumeOperations：查找與回收

- (void)lookupItemNamed:(FSFileName *)name
            inDirectory:(FSItem *)directory
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *dir = [self ntfsItem:directory];
  NSString *nameString = NTFSStringFromName(name);
  if (!dir || !nameString) {
    reply(nil, nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(nil, nil, NTFSError(ENXIO));
    return;
  }
  uint64_t mft = 0;
  ntfsb_type type = NTFSB_TYPE_UNKNOWN;
  int rc = ntfsb_lookup(_vol, dir.mftNo, nameString.UTF8String, &mft, &type);
  NTFSItem *item = rc ? nil : [self itemForMFTNo:mft type:type];
  [self.lock unlock];
  if (rc) {
    reply(nil, nil, NTFSError(rc));
    return;
  }
  reply(item, name, nil);
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (ni) {
    [self.lock lock];
    if (self.items[@(ni.mftNo)] == ni && ni != self.rootItem) {
      [self.items removeObjectForKey:@(ni.mftNo)];
    }
    [self.lock unlock];
  }
  reply(nil);
}

- (void)readSymbolicLink:(FSItem *)item
            replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  char buf[PATH_MAX + 1];
  [self.lock lock];
  int rc = _vol ? ntfsb_readlink(_vol, ni.mftNo, buf, sizeof(buf)) : -ENXIO;
  [self.lock unlock];
  if (rc < 0) {
    reply(nil, NTFSError(rc));
    return;
  }
  reply([FSFileName nameWithString:[NSString stringWithUTF8String:buf] ?: @""], nil);
}

// MARK: - FSVolumeOperations：建立 / 刪除 / 連結 / 重新命名

- (ntfsb_type)bridgeTypeForItemType:(FSItemType)type {
  switch (type) {
    case FSItemTypeFile: return NTFSB_TYPE_FILE;
    case FSItemTypeDirectory: return NTFSB_TYPE_DIR;
    case FSItemTypeSymlink: return NTFSB_TYPE_SYMLINK;
    case FSItemTypeFIFO: return NTFSB_TYPE_FIFO;
    case FSItemTypeSocket: return NTFSB_TYPE_SOCK;
    default: return NTFSB_TYPE_UNKNOWN;
  }
}

/// 建立後套用初始屬性（時間、旗標）
- (void)applyInitialAttributes:(FSItemSetAttributesRequest *_Nullable)attrs toMFTNo:(uint64_t)mft {
  if (!attrs) {
    return;
  }
  FSItemAttribute consumed = 0;
  struct timespec atime, mtime, btime;
  const struct timespec *pa = NULL, *pm = NULL, *pb = NULL;
  if ([attrs isValid:FSItemAttributeAccessTime]) {
    atime = attrs.accessTime;
    pa = &atime;
    consumed |= FSItemAttributeAccessTime;
  }
  if ([attrs isValid:FSItemAttributeModifyTime]) {
    mtime = attrs.modifyTime;
    pm = &mtime;
    consumed |= FSItemAttributeModifyTime;
  }
  if ([attrs isValid:FSItemAttributeBirthTime]) {
    btime = attrs.birthTime;
    pb = &btime;
    consumed |= FSItemAttributeBirthTime;
  }
  if (pa || pm || pb) {
    ntfsb_set_times(_vol, mft, pa, pm, NULL, pb);
  }
  if ([attrs isValid:FSItemAttributeFlags] && (attrs.flags & UF_HIDDEN)) {
    ntfsb_set_flags(_vol, mft, -1, 1);
    consumed |= FSItemAttributeFlags;
  }
  if ([attrs isValid:FSItemAttributeMode]) {
    if (!(attrs.mode & 0200)) {
      ntfsb_set_flags(_vol, mft, 1, -1);
    }
    consumed |= FSItemAttributeMode;
  }
  if ([attrs isValid:FSItemAttributeUID]) {
    consumed |= FSItemAttributeUID;
  }
  if ([attrs isValid:FSItemAttributeGID]) {
    consumed |= FSItemAttributeGID;
  }
  attrs.consumedAttributes = consumed;
}

- (void)createItemNamed:(FSFileName *)name
                   type:(FSItemType)type
            inDirectory:(FSItem *)directory
             attributes:(FSItemSetAttributesRequest *)newAttributes
           replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *dir = [self ntfsItem:directory];
  NSString *nameString = NTFSStringFromName(name);
  ntfsb_type btype = [self bridgeTypeForItemType:type];
  if (!dir || !nameString || btype == NTFSB_TYPE_UNKNOWN || btype == NTFSB_TYPE_SYMLINK) {
    reply(nil, nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(nil, nil, NTFSError(ENXIO));
    return;
  }
  uint64_t mft = 0;
  int rc = ntfsb_create(_vol, dir.mftNo, nameString.UTF8String, btype, NULL, &mft);
  NTFSItem *item = nil;
  if (!rc) {
    [self applyInitialAttributes:newAttributes toMFTNo:mft];
    item = [self itemForMFTNo:mft type:btype];
    dir.generation++;
  }
  [self.lock unlock];
  if (rc) {
    reply(nil, nil, NTFSError(rc));
    return;
  }
  reply(item, name, nil);
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
                    inDirectory:(FSItem *)directory
                     attributes:(FSItemSetAttributesRequest *)newAttributes
                   linkContents:(FSFileName *)contents
                   replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *dir = [self ntfsItem:directory];
  NSString *nameString = NTFSStringFromName(name);
  NSString *target = NTFSStringFromName(contents);
  if (!dir || !nameString || !target) {
    reply(nil, nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(nil, nil, NTFSError(ENXIO));
    return;
  }
  uint64_t mft = 0;
  int rc = ntfsb_create(_vol, dir.mftNo, nameString.UTF8String, NTFSB_TYPE_SYMLINK, target.UTF8String, &mft);
  NTFSItem *item = nil;
  if (!rc) {
    [self applyInitialAttributes:newAttributes toMFTNo:mft];
    item = [self itemForMFTNo:mft type:NTFSB_TYPE_SYMLINK];
    dir.generation++;
  }
  [self.lock unlock];
  if (rc) {
    reply(nil, nil, NTFSError(rc));
    return;
  }
  reply(item, name, nil);
}

- (void)createLinkToItem:(FSItem *)item
                   named:(FSFileName *)name
             inDirectory:(FSItem *)directory
            replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  NTFSItem *dir = [self ntfsItem:directory];
  NSString *nameString = NTFSStringFromName(name);
  if (!ni || !dir || !nameString) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  if (ni.type == NTFSB_TYPE_DIR) {
    reply(nil, NTFSError(EPERM));
    return;
  }
  [self.lock lock];
  int rc = _vol ? ntfsb_link(_vol, ni.mftNo, dir.mftNo, nameString.UTF8String) : -ENXIO;
  if (!rc) {
    dir.generation++;
  }
  [self.lock unlock];
  reply(rc ? nil : name, rc ? NTFSError(rc) : nil);
}

- (void)removeItem:(FSItem *)item
             named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  NTFSItem *dir = [self ntfsItem:directory];
  NSString *nameString = NTFSStringFromName(name);
  if (!ni || !dir || !nameString) {
    reply(NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int rc = _vol ? ntfsb_unlink(_vol, dir.mftNo, nameString.UTF8String, ni.mftNo) : -ENXIO;
  if (!rc) {
    dir.generation++;
  }
  [self.lock unlock];
  reply(rc ? NTFSError(rc) : nil);
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
             named:(FSFileName *)sourceName
         toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
          overItem:(FSItem *_Nullable)overItem
      replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  NTFSItem *srcDir = [self ntfsItem:sourceDirectory];
  NTFSItem *dstDir = [self ntfsItem:destinationDirectory];
  NTFSItem *over = overItem ? [self ntfsItem:overItem] : nil;
  NSString *srcName = NTFSStringFromName(sourceName);
  NSString *dstName = NTFSStringFromName(destinationName);
  if (!ni || !srcDir || !dstDir || !srcName || !dstName || (overItem && !over)) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int rc = _vol ? ntfsb_rename(_vol, srcDir.mftNo, srcName.UTF8String, ni.mftNo,
                               dstDir.mftNo, dstName.UTF8String, over ? over.mftNo : 0)
                : -ENXIO;
  if (!rc) {
    srcDir.generation++;
    dstDir.generation++;
  }
  [self.lock unlock];
  reply(rc ? nil : destinationName, rc ? NTFSError(rc) : nil);
}

// MARK: - FSVolumeOperations：目錄列舉

/// 列舉過程狀態
typedef struct NTFSEnumContext {
  void *volume;                 ///< __bridge NTFSVolume *
  void *packer;                 ///< __bridge FSDirectoryEntryPacker *
  void *request;                ///< __bridge FSItemGetAttributesRequest * 或 NULL
  BOOL includeDots;
  int64_t lastNextCookie;
  int packed;
} NTFSEnumContext;

static int NTFSEnumCallback(void *context, const char *name, size_t len, uint64_t mft,
                            ntfsb_type type, int64_t cookie, int64_t next) {
  NTFSEnumContext *ctx = context;
  NTFSVolume *volume = (__bridge NTFSVolume *)ctx->volume;
  FSDirectoryEntryPacker *packer = (__bridge FSDirectoryEntryPacker *)ctx->packer;
  FSItemGetAttributesRequest *request = (__bridge FSItemGetAttributesRequest *)ctx->request;
  BOOL isDot = (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
  if (isDot && !ctx->includeDots) {
    ctx->lastNextCookie = next;
    return 0;
  }
  FSFileName *fsName = [FSFileName nameWithData:[NSData dataWithBytes:name length:len]];
  FSItemAttributes *attrs = nil;
  if (request) {
    ntfsb_attr a;
    if (ntfsb_getattr(volume->_vol, mft, &a) == 0) {
      type = a.type;
      attrs = [volume attributesFromBridge:&a requested:request];
    }
  }
  BOOL ok = [packer packEntryWithName:fsName
                             itemType:[NTFSItem fsItemTypeForType:type]
                               itemID:[NTFSItem itemIDForMFTNo:mft]
                           nextCookie:(FSDirectoryCookie)next
                           attributes:attrs];
  if (!ok) {
    return 1;
  }
  ctx->packed++;
  ctx->lastNextCookie = next;
  return 0;
}

- (void)enumerateDirectory:(FSItem *)directory
          startingAtCookie:(FSDirectoryCookie)cookie
                  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *_Nullable)attributes
               usingPacker:(FSDirectoryEntryPacker *)packer
              replyHandler:(void (^)(FSDirectoryVerifier, NSError *_Nullable))reply {
  NTFSItem *dir = [self ntfsItem:directory];
  if (!dir) {
    reply(0, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(0, NTFSError(ENXIO));
    return;
  }
  NTFSEnumContext ctx = {
    .volume = (__bridge void *)self,
    .packer = (__bridge void *)packer,
    .request = (__bridge void *)attributes,
    .includeDots = (attributes == nil),
    .lastNextCookie = (int64_t)cookie,
  };
  int64_t pos = (int64_t)cookie;
  int rc = ntfsb_readdir(_vol, dir.mftNo, &pos, NTFSEnumCallback, &ctx);
  FSDirectoryVerifier current = (FSDirectoryVerifier)dir.generation;
  [self.lock unlock];
  if (rc) {
    reply(current, NTFSError(rc));
    return;
  }
  reply(current, nil);
}

// MARK: - FSVolumeOpenCloseOperations

- (void)openItem:(FSItem *)item withModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(NTFSError(EINVAL));
    return;
  }
  if ((modes & FSVolumeOpenModesWrite) && self.readOnly) {
    reply(NTFSError(EROFS));
    return;
  }
  [self.lock lock];
  ni.openCount++;
  [self.lock unlock];
  reply(nil);
}

- (void)closeItem:(FSItem *)item keepingModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (ni.openCount > 0) {
    ni.openCount--;
  }
  [self.lock unlock];
  reply(nil);
}

// MARK: - FSVolumeReadWriteOperations

- (void)readFromFile:(FSItem *)item
              offset:(off_t)offset
              length:(size_t)length
          intoBuffer:(FSMutableFileDataBuffer *)buffer
        replyHandler:(void (^)(size_t, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(0, NTFSError(EINVAL));
    return;
  }
  size_t want = MIN(length, buffer.length);
  [self.lock lock];
  int64_t got = _vol ? ntfsb_read(_vol, ni.mftNo, offset, buffer.mutableBytes, want) : -ENXIO;
  [self.lock unlock];
  if (got < 0) {
    reply(0, NTFSError((int)got));
    return;
  }
  reply((size_t)got, nil);
}

- (void)writeContents:(NSData *)contents
               toFile:(FSItem *)item
             atOffset:(off_t)offset
         replyHandler:(void (^)(size_t, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(0, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int64_t put = _vol ? ntfsb_write(_vol, ni.mftNo, offset, contents.bytes, contents.length) : -ENXIO;
  [self.lock unlock];
  if (put < 0) {
    reply(0, NTFSError((int)put));
    return;
  }
  reply((size_t)put, nil);
}

// MARK: - FSVolumeXattrOperations

- (void)getXattrNamed:(FSFileName *)name
               ofItem:(FSItem *)item
         replyHandler:(void (^)(NSData *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  NSString *nameString = NTFSStringFromName(name);
  if (!ni || !nameString) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  if (!_vol) {
    [self.lock unlock];
    reply(nil, NTFSError(ENXIO));
    return;
  }
  int64_t size = ntfsb_xattr_get(_vol, ni.mftNo, nameString.UTF8String, NULL, 0);
  if (size < 0) {
    [self.lock unlock];
    reply(nil, NTFSError((int)size));
    return;
  }
  NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)size];
  if (size > 0) {
    int64_t got = ntfsb_xattr_get(_vol, ni.mftNo, nameString.UTF8String, data.mutableBytes, (size_t)size);
    if (got < 0) {
      [self.lock unlock];
      reply(nil, NTFSError((int)got));
      return;
    }
    data.length = (NSUInteger)got;
  }
  [self.lock unlock];
  reply(data, nil);
}

- (void)setXattrNamed:(FSFileName *)name
               toData:(NSData *_Nullable)value
               onItem:(FSItem *)item
               policy:(FSSetXattrPolicy)policy
         replyHandler:(void (^)(NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  NSString *nameString = NTFSStringFromName(name);
  if (!ni || !nameString) {
    reply(NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int rc;
  if (!_vol) {
    rc = -ENXIO;
  } else if (policy == FSSetXattrPolicyDelete || value == nil) {
    rc = ntfsb_xattr_remove(_vol, ni.mftNo, nameString.UTF8String);
  } else {
    ntfsb_xattr_policy p = NTFSB_XATTR_ALWAYS_SET;
    if (policy == FSSetXattrPolicyMustCreate) {
      p = NTFSB_XATTR_MUST_CREATE;
    } else if (policy == FSSetXattrPolicyMustReplace) {
      p = NTFSB_XATTR_MUST_REPLACE;
    }
    rc = ntfsb_xattr_set(_vol, ni.mftNo, nameString.UTF8String, value.bytes, value.length, p);
  }
  [self.lock unlock];
  reply(rc ? NTFSError(rc) : nil);
}

static int NTFSXattrListCallback(void *context, const char *name, size_t len) {
  NSMutableArray<FSFileName *> *names = (__bridge NSMutableArray *)context;
  [names addObject:[FSFileName nameWithData:[NSData dataWithBytes:name length:len]]];
  return 0;
}

- (void)listXattrsOfItem:(FSItem *)item
            replyHandler:(void (^)(NSArray<FSFileName *> *_Nullable, NSError *_Nullable))reply {
  NTFSItem *ni = [self ntfsItem:item];
  if (!ni) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  NSMutableArray<FSFileName *> *names = [NSMutableArray array];
  [self.lock lock];
  int rc = _vol ? ntfsb_xattr_list(_vol, ni.mftNo, NTFSXattrListCallback, (__bridge void *)names) : -ENXIO;
  [self.lock unlock];
  reply(rc ? nil : names, rc ? NTFSError(rc) : nil);
}

// MARK: - FSVolumeRenameOperations

- (void)setVolumeName:(FSFileName *)name
         replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply {
  NSString *label = NTFSStringFromName(name);
  if (!label) {
    reply(nil, NTFSError(EINVAL));
    return;
  }
  [self.lock lock];
  int rc = _vol ? ntfsb_set_label(_vol, label.UTF8String) : -ENXIO;
  if (!rc) {
    self.name = name;
  }
  [self.lock unlock];
  reply(rc ? nil : name, rc ? NTFSError(rc) : nil);
}

@end
