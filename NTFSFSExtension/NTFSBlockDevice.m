//
//  NTFSBlockDevice.m
//  NTFSFSExtension
//

#import "NTFSBlockDevice.h"
#import <os/log.h>
#import <errno.h>

static os_log_t NTFSDeviceLog(void) {
  static os_log_t log;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    log = os_log_create("com.lintayu.MacNTFS", "device");
  });
  return log;
}

/// 把 NSError 轉成 errno 值
static int NTFSErrnoFromError(NSError *error) {
  if (!error) {
    return EIO;
  }
  if ([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code > 0) {
    return (int)error.code;
  }
  return EIO;
}

// MARK: - C 回呼

static int64_t NTFSDevicePread(void *context, void *buf, int64_t count, int64_t offset) {
  NTFSBlockDevice *device = (__bridge NTFSBlockDevice *)context;
  NSError *error = nil;
  size_t got = [device.resource readInto:buf startingAt:offset length:(size_t)count error:&error];
  if (error) {
    os_log_error(NTFSDeviceLog(), "讀取裝置失敗 offset=%lld length=%lld: %{public}@", offset, count, error);
    errno = NTFSErrnoFromError(error);
    return -1;
  }
  return (int64_t)got;
}

static int64_t NTFSDevicePwrite(void *context, const void *buf, int64_t count, int64_t offset) {
  NTFSBlockDevice *device = (__bridge NTFSBlockDevice *)context;
  if (!device.resource.writable) {
    errno = EROFS;
    return -1;
  }
  NSError *error = nil;
  size_t put = [device.resource writeFrom:(void *)buf startingAt:offset length:(size_t)count error:&error];
  if (error) {
    os_log_error(NTFSDeviceLog(), "寫入裝置失敗 offset=%lld length=%lld: %{public}@", offset, count, error);
    errno = NTFSErrnoFromError(error);
    return -1;
  }
  return (int64_t)put;
}

static int NTFSDeviceSync(void *context) {
  NTFSBlockDevice *device = (__bridge NTFSBlockDevice *)context;
  // 我們只使用直接 I/O，資料在 write 回傳時就已送達裝置；
  // 這裡順便把 FSKit 端的中繼資料快取排空以確保一致。
  NSError *error = nil;
  if (![device.resource metadataFlushWithError:&error]) {
    os_log_error(NTFSDeviceLog(), "metadataFlush 失敗: %{public}@", error);
  }
  return 0;
}

// MARK: -

@implementation NTFSBlockDevice {
  ntfsb_device_callbacks _callbacks;
}

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource {
  self = [super init];
  if (self) {
    _resource = resource;
    _callbacks.context = (__bridge void *)self;
    _callbacks.block_size = (uint32_t)resource.blockSize;
    _callbacks.block_count = resource.blockCount;
    _callbacks.writable = resource.writable;
    _callbacks.pread = NTFSDevicePread;
    _callbacks.pwrite = NTFSDevicePwrite;
    _callbacks.sync = NTFSDeviceSync;
    os_log_info(NTFSDeviceLog(), "裝置 %{public}@ blockSize=%llu blockCount=%llu writable=%d",
                resource.BSDName, resource.blockSize, resource.blockCount, resource.writable);
  }
  return self;
}

- (ntfsb_device_callbacks)callbacks {
  return _callbacks;
}

@end
