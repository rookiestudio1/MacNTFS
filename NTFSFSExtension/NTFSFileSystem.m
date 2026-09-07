//
//  NTFSFileSystem.m
//  NTFSFSExtension
//

#import "NTFSFileSystem.h"
#import "NTFSBlockDevice.h"
#import "NTFSVolume.h"
#import "ntfs_bridge.h"
#import <os/log.h>
#import <errno.h>

static os_log_t NTFSFSLog(void) {
  static os_log_t log;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    log = os_log_create("com.rookiestudio.MacNTFS", "filesystem");
  });
  return log;
}

/// 由 NTFS 卷序號產生穩定的 UUID（同一顆卷每次探測都得到相同 ID）
static NSUUID *NTFSUUIDFromSerial(uint64_t serial) {
  uuid_t bytes;
  memset(bytes, 0, sizeof(bytes));
  memcpy(bytes, &serial, sizeof(serial));
  bytes[8] = 'N';
  bytes[9] = 'T';
  bytes[10] = 'F';
  bytes[11] = 'S';
  bytes[12] = (uint8_t)(serial >> 56);
  bytes[13] = (uint8_t)(serial >> 40);
  bytes[14] = (uint8_t)(serial >> 24);
  bytes[15] = (uint8_t)(serial >> 8);
  // 標成 RFC 4122 第 4 版格式
  bytes[6] = (bytes[6] & 0x0F) | 0x40;
  bytes[8] = (bytes[8] & 0x3F) | 0x80;
  return [[NSUUID alloc] initWithUUIDBytes:bytes];
}

@interface NTFSFileSystem ()
@property (nonatomic, strong) NSMutableDictionary<NSString *, NTFSVolume *> *volumesByBSDName;
@property (nonatomic, strong) NSLock *volumesLock;
/// 最近一次 loadResource 收到的區塊裝置；fsck / format 會針對它操作（與 Apple msdosfs 相同做法）
@property (nonatomic, strong, nullable) FSBlockDeviceResource *lastResource;
@end

@implementation NTFSFileSystem

- (instancetype)init {
  self = [super init];
  if (self) {
    ntfsb_global_init();
    _volumesByBSDName = [NSMutableDictionary dictionary];
    _volumesLock = [[NSLock alloc] init];
    self.containerStatus = FSContainerStatus.ready;
  }
  return self;
}

- (void)didFinishLoading {
  self.containerStatus = FSContainerStatus.ready;
}

// MARK: - 探測

- (void)probeResource:(FSResource *)resource
         replyHandler:(void (^)(FSProbeResult *_Nullable, NSError *_Nullable))reply {
  if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
    reply(FSProbeResult.notRecognizedProbeResult, nil);
    return;
  }
  FSBlockDeviceResource *block = (FSBlockDeviceResource *)resource;
  NTFSBlockDevice *device = [[NTFSBlockDevice alloc] initWithResource:block];
  ntfsb_device_callbacks cb = device.callbacks;
  ntfsb_probe_info info;
  int rc = ntfsb_probe(&cb, &info);
  if (rc == -EINVAL) {
    os_log_info(NTFSFSLog(), "%{public}@ 不是 NTFS", block.BSDName);
    reply(FSProbeResult.notRecognizedProbeResult, nil);
    return;
  }
  if (rc < 0) {
    os_log_error(NTFSFSLog(), "%{public}@ 是 NTFS 但無法解析 (rc=%d)", block.BSDName, rc);
    FSContainerIdentifier *cid = [[FSContainerIdentifier alloc] initWithUUID:[NSUUID UUID]];
    reply([FSProbeResult recognizedProbeResultWithName:@"" containerID:cid], nil);
    return;
  }
  NSString *label = [NSString stringWithUTF8String:info.label] ?: @"";
  FSContainerIdentifier *cid = [[FSContainerIdentifier alloc] initWithUUID:NTFSUUIDFromSerial(info.serial)];
  os_log_info(NTFSFSLog(), "%{public}@ NTFS %d.%d 卷標=\"%{public}@\" dirty=%d hibernated=%d",
              block.BSDName, info.major, info.minor, label, info.dirty, info.hibernated);
  // 休眠中的卷仍回報 usable：掛載時 libntfs-3g 會自動退回唯讀
  reply([FSProbeResult usableProbeResultWithName:label containerID:cid], nil);
}

// MARK: - 載入 / 卸載

- (void)loadResource:(FSResource *)resource
             options:(FSTaskOptions *)options
        replyHandler:(void (^)(FSVolume *_Nullable, NSError *_Nullable))reply {
  if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
    reply(nil, fs_errorForPOSIXError(EINVAL));
    return;
  }
  FSBlockDeviceResource *block = (FSBlockDeviceResource *)resource;
  NTFSBlockDevice *device = [[NTFSBlockDevice alloc] initWithResource:block];
  ntfsb_device_callbacks cb = device.callbacks;
  ntfsb_probe_info info;
  int rc = ntfsb_probe(&cb, &info);
  if (rc < 0) {
    os_log_error(NTFSFSLog(), "載入 %{public}@ 失敗：probe rc=%d", block.BSDName, rc);
    reply(nil, fs_errorForPOSIXError(rc == -EINVAL ? EINVAL : -rc));
    return;
  }
  NSString *label = [NSString stringWithUTF8String:info.label] ?: @"";
  // volumeID 必須與 probe 回報的 containerID 相同
  FSVolumeIdentifier *vid = [[FSVolumeIdentifier alloc] initWithUUID:NTFSUUIDFromSerial(info.serial)];
  NTFSVolume *volume = [[NTFSVolume alloc] initWithDevice:device
                                                 volumeID:vid
                                               volumeName:[FSFileName nameWithString:label.length ? label : @"NTFS"]];
  [self.volumesLock lock];
  self.volumesByBSDName[block.BSDName] = volume;
  self.lastResource = block;
  [self.volumesLock unlock];
  self.containerStatus = FSContainerStatus.ready;
  os_log_info(NTFSFSLog(), "已載入 %{public}@（%{public}@）", block.BSDName, label);
  reply(volume, nil);
}

- (void)unloadResource:(FSResource *)resource
               options:(FSTaskOptions *)options
          replyHandler:(void (^)(NSError *_Nullable))reply {
  if ([resource isKindOfClass:FSBlockDeviceResource.class]) {
    NSString *bsd = ((FSBlockDeviceResource *)resource).BSDName;
    [self.volumesLock lock];
    NTFSVolume *volume = self.volumesByBSDName[bsd];
    [self.volumesByBSDName removeObjectForKey:bsd];
    if ([self.lastResource.BSDName isEqualToString:bsd]) {
      self.lastResource = nil;
    }
    BOOL empty = self.volumesByBSDName.count == 0;
    [self.volumesLock unlock];
    [volume unmountIfNeeded];
    if (empty) {
      self.containerStatus = FSContainerStatus.ready;
    }
    os_log_info(NTFSFSLog(), "已卸載 %{public}@", bsd);
  }
  reply(nil);
}

// MARK: - FSManageableResourceMaintenanceOperations

- (NSProgress *_Nullable)startCheckWithTask:(FSTask *)task
                                    options:(FSTaskOptions *)options
                                      error:(NSError **)error {
  // DiskArbitration 在自動掛載前會以 -q 呼叫 fsck；我們以「能否唯讀解析」作為檢查結果。
  NSProgress *progress = [NSProgress progressWithTotalUnitCount:1];
  BOOL quick = [options.taskOptions containsObject:@"-q"];
  [self.volumesLock lock];
  FSBlockDeviceResource *block = self.lastResource;
  [self.volumesLock unlock];
  // 必須非同步完成，否則 fskitd 會回報 FSKitErrorDomain 27503
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    NSError *result = nil;
    if (block) {
      NTFSBlockDevice *device = [[NTFSBlockDevice alloc] initWithResource:block];
      ntfsb_device_callbacks cb = device.callbacks;
      ntfsb_probe_info info;
      int rc = ntfsb_probe(&cb, &info);
      if (rc < 0) {
        [task logMessage:[NSString stringWithFormat:@"無法解析 NTFS 卷 (errno %d)", -rc]];
        result = fs_errorForPOSIXError(-rc);
      } else if (info.hibernated) {
        [task logMessage:@"Windows 處於休眠狀態，請在 Windows 完整關機後再試；目前只能唯讀掛載"];
        result = quick ? nil : fs_errorForPOSIXError(EPERM);
      } else if (info.dirty) {
        [task logMessage:@"卷未正常卸載，掛載時將重設 NTFS 日誌"];
      } else {
        [task logMessage:@"NTFS 卷檢查通過"];
      }
    } else {
      [task logMessage:@"尚未載入任何 NTFS 資源，無法檢查"];
      result = fs_errorForPOSIXError(ENXIO);
    }
    progress.completedUnitCount = 1;
    [task didCompleteWithError:result];
  });
  return progress;
}

- (NSProgress *_Nullable)startFormatWithTask:(FSTask *)task
                                     options:(FSTaskOptions *)options
                                       error:(NSError **)error {
  if (error) {
    *error = fs_errorForPOSIXError(ENOTSUP);
  }
  return nil;
}

- (void)wipeResource:(FSBlockDeviceResource *)resource
   completionHandler:(void (^)(NSError *_Nullable))completion {
  completion(fs_errorForPOSIXError(ENOTSUP));
}

@end
