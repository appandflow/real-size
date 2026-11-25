#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "real-size.h"

enum volume { LINK_COUNTED, EXTENT_MAPPED };

/* statfs(2) f_type magic numbers from linux/magic.h. On ext2/3/4 and tmpfs files share blocks only through hard links.
 * Btrfs and XFS can share extents between files that have one link each (reflinks), which FIEMAP reports. */
static int classify(int fd, enum volume *volume) {
  struct statfs info;
  if (fstatfs(fd, &info) < 0) return -1;
  switch ((uint32_t)info.f_type) {
  case 0xef53u:
  case 0x01021994u:
    *volume = LINK_COUNTED;
    return 0;
  case 0x9123683eu:
  case 0x58465342u:
    *volume = EXTENT_MAPPED;
    return 0;
  default:
    return UNSUPPORTED;
  }
}

/* FS_IOC_FIEMAP flags an extent FIEMAP_EXTENT_SHARED when another file references it. fe_length is the logical length,
 * which exceeds the allocation of a compressed extent, so a file without shared extents reports st_blocks. A file the
 * walk cannot open or map counts as fully shared. */
static uint64_t private_bytes(int dirfd, const char *name, uint64_t allocated, int *shared) {
  int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) {
    *shared = 1;
    return 0;
  }
  struct {
    struct fiemap map;
    struct fiemap_extent extents[64];
  } request;
  uint64_t start = 0, unshared = 0;
  *shared = 0;
  for (;;) {
    memset(&request, 0, sizeof request);
    request.map.fm_start = start;
    request.map.fm_length = FIEMAP_MAX_OFFSET;
    request.map.fm_extent_count = 64;
    if (ioctl(fd, FS_IOC_FIEMAP, &request.map) < 0) {
      *shared = 1;
      unshared = 0;
      break;
    }
    uint32_t mapped = request.map.fm_mapped_extents;
    if (mapped == 0) break;
    int last = 0;
    for (uint32_t i = 0; i < mapped; i++) {
      const struct fiemap_extent *extent = &request.extents[i];
      if (extent->fe_flags & FIEMAP_EXTENT_SHARED) *shared = 1;
      else unshared += extent->fe_length;
      if (extent->fe_flags & FIEMAP_EXTENT_LAST) last = 1;
    }
    const struct fiemap_extent *tail = &request.extents[mapped - 1];
    if (last || tail->fe_logical + tail->fe_length <= start) break;
    start = tail->fe_logical + tail->fe_length;
  }
  close(fd);
  if (!*shared) return allocated;
  return unshared < allocated ? unshared : allocated;
}

static int walk(int dirfd, dev_t dev, enum volume volume, struct totals *totals, int depth) {
  DIR *dir = fdopendir(dirfd);
  if (!dir) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return -1;
  }
  int result = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno) result = -1;
      break;
    }
    const char *name = entry->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    struct stat info;
    if (fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
      if (errno == ENOENT) continue;
      result = -1;
      break;
    }
    if (S_ISDIR(info.st_mode)) {
      if (depth + 1 >= MAX_DEPTH) {
        errno = ELOOP;
        result = -1;
        break;
      }
      int child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child < 0) {
        if (errno == ENOENT) continue;
        result = -1;
        break;
      }
      enum volume child_volume = volume;
      if (info.st_dev != dev) {
        result = classify(child, &child_volume);
        if (result < 0) {
          int saved = errno;
          close(child);
          errno = saved;
          break;
        }
      }
      result = walk(child, info.st_dev, child_volume, totals, depth + 1);
      if (result < 0) break;
    } else if (S_ISREG(info.st_mode)) {
      uint64_t allocated = (uint64_t)info.st_blocks * 512;
      uint64_t private_size = allocated;
      int shared = 0;
      if (volume == EXTENT_MAPPED && allocated > 0) private_size = private_bytes(dirfd, name, allocated, &shared);
      add_file(totals, (uint64_t)info.st_dev, (uint64_t)info.st_ino, 0, (uint32_t)info.st_nlink, allocated,
               private_size, shared);
    }
  }
  int saved = errno;
  closedir(dir);
  errno = saved;
  return result;
}

int measure(const char *path, struct totals *totals) {
  int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0) return -1;
  struct stat info;
  enum volume volume = LINK_COUNTED;
  int classified = fstat(dirfd, &info) < 0 ? -1 : classify(dirfd, &volume);
  if (classified < 0) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return classified;
  }
  return walk(dirfd, info.st_dev, volume, totals, 0);
}

const char *measure_error(void) { return strerror(errno); }

int main(int argc, char **argv) { return real_size_main(argc, argv); }
