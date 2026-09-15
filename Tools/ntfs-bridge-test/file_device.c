//
//  file_device.c
//  ntfs-bridge-test
//

#include "file_device.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int64_t file_pread(void *ctx, void *buf, int64_t count, int64_t offset) {
  file_device *d = ctx;
  ssize_t r = pread(d->fd, buf, (size_t)count, offset);
  if (r < 0) {
    return -1;
  }
  d->stats.read_calls++;
  d->stats.read_bytes += (uint64_t)r;
  if (d->trace) {
    fprintf(d->trace, "r %lld %lld\n", (long long)offset, (long long)r);
  }
  return (int64_t)r;
}

static int64_t file_pwrite(void *ctx, const void *buf, int64_t count, int64_t offset) {
  file_device *d = ctx;
  ssize_t r = pwrite(d->fd, buf, (size_t)count, offset);
  if (r < 0) {
    return -1;
  }
  d->stats.write_calls++;
  d->stats.write_bytes += (uint64_t)r;
  if (d->trace) {
    fprintf(d->trace, "w %lld %lld\n", (long long)offset, (long long)r);
  }
  return (int64_t)r;
}

static int file_sync(void *ctx) {
  file_device *d = ctx;
  d->stats.sync_calls++;
  return fsync(d->fd);
}

int file_device_open(file_device *d, const char *path) {
  memset(d, 0, sizeof(*d));
  d->fd = open(path, O_RDWR);
  if (d->fd < 0) {
    return -1;
  }
  struct stat st;
  if (fstat(d->fd, &st) < 0) {
    close(d->fd);
    d->fd = -1;
    return -1;
  }
  d->size = st.st_size;
  return 0;
}

void file_device_close(file_device *d) {
  if (d->fd >= 0) {
    close(d->fd);
    d->fd = -1;
  }
}

ntfsb_device_callbacks file_device_callbacks(file_device *d, uint32_t block_size) {
  ntfsb_device_callbacks cb = {
    .context = d,
    .block_size = block_size,
    .block_count = (uint64_t)d->size / block_size,
    .writable = true,
    .pread = file_pread,
    .pwrite = file_pwrite,
    .sync = file_sync,
  };
  return cb;
}

int file_device_trace(file_device *d, const char *path) {
  if (d->trace) {
    fclose(d->trace);
    d->trace = NULL;
  }
  if (!path) {
    return 0;
  }
  d->trace = fopen(path, "w");
  return d->trace ? 0 : -1;
}

void file_device_reset_stats(file_device *d) {
  memset(&d->stats, 0, sizeof(d->stats));
}
