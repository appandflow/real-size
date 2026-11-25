#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "real-size.h"

enum volume { LINK_COUNTED, IN_MEMORY, EXTENT_MAPPED };

#ifndef RS_SORT_THRESHOLD
#define RS_SORT_THRESHOLD 10000
#endif
#ifndef RS_TYPE_SKIP
#define RS_TYPE_SKIP 1
#endif

/* statfs(2) f_type magic numbers from linux/magic.h. On ext2/3/4 and tmpfs files share blocks only through hard links.
 * Btrfs and XFS can share extents between files that have one link each (reflinks), which FIEMAP reports. */
static int classify(int fd, enum volume *volume) {
  struct statfs info;
  if (fstatfs(fd, &info) < 0) return -1;
  switch ((uint32_t)info.f_type) {
  case 0xef53u:
    *volume = LINK_COUNTED;
    return 0;
  case 0x01021994u:
    *volume = IN_MEMORY;
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

/* A file with several links on btrfs or XFS is mapped only after the walk, and only when every link was inside the
 * tree; until then it is known by its path relative to the measured directory. */
static char relative[PATH_MAX];

static int walk(int dirfd, dev_t dev, enum volume volume, struct totals *totals, int depth, size_t length);

struct entry {
  ino_t ino;
  size_t name;
};

static int by_inode(const void *a, const void *b) {
  ino_t x = ((const struct entry *)a)->ino, y = ((const struct entry *)b)->ino;
  return x < y ? -1 : x > y;
}

static int visit(int dirfd, const char *name, dev_t dev, enum volume volume, struct totals *totals, int depth,
                 size_t length) {
  size_t name_length = strlen(name);
  size_t child_length = length + name_length + 1;
  if (child_length < sizeof relative) {
    relative[length] = '/';
    memcpy(relative + length + 1, name, name_length + 1);
  }
  struct stat info;
  if (fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) < 0) return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(info.st_mode)) {
    if (depth + 1 >= MAX_DEPTH) {
      errno = ELOOP;
      return -1;
    }
    int child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) return errno == ENOENT ? 0 : -1;
    enum volume child_volume = volume;
    if (info.st_dev != dev) {
      int classified = classify(child, &child_volume);
      if (classified < 0) {
        int saved = errno;
        close(child);
        errno = saved;
        return classified;
      }
    }
    return walk(child, info.st_dev, child_volume, totals, depth + 1, child_length);
  }
  if (!S_ISREG(info.st_mode)) return 0;
  uint64_t allocated = (uint64_t)info.st_blocks * 512;
  uint64_t private_size = allocated;
  int shared = 0;
  int defer = volume == EXTENT_MAPPED && allocated > 0 && info.st_nlink > 1 && child_length < sizeof relative;
  if (volume == EXTENT_MAPPED && allocated > 0 && !defer) private_size = private_bytes(dirfd, name, allocated, &shared);
  struct inode *linked = add_file(totals, (uint64_t)info.st_dev, (uint64_t)info.st_ino, 0, (uint32_t)info.st_nlink,
                                  allocated, private_size, shared);
  if (linked && defer) {
    linked->deferred = strdup(relative + 1);
    if (!linked->deferred) {
      errno = ENOMEM;
      return -1;
    }
  }
  return 0;
}

/* Like GNU du through gnulib fts (lib/fts.c, fts_build and dirent_inode_sort_may_be_useful), a directory with at
 * least RS_SORT_THRESHOLD entries is read in full and its entries are stat'ed in inode order, which on ext4 reads the
 * inode table in order on a cold cache; tmpfs gains nothing from it. */
static int walk(int dirfd, dev_t dev, enum volume volume, struct totals *totals, int depth, size_t length) {
  DIR *dir = fdopendir(dirfd);
  if (!dir) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return -1;
  }
  struct entry *entries = NULL;
  char *names = NULL;
  size_t count = 0, entry_capacity = 0, names_length = 0, names_capacity = 0;
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
#if RS_TYPE_SKIP
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR && entry->d_type != DT_REG) continue;
#endif
    size_t size = strlen(name) + 1;
    if (count == entry_capacity) {
      entry_capacity = entry_capacity ? entry_capacity * 2 : 64;
      struct entry *grown = realloc(entries, entry_capacity * sizeof *entries);
      if (!grown) {
        errno = ENOMEM;
        result = -1;
        break;
      }
      entries = grown;
    }
    if (names_length + size > names_capacity) {
      names_capacity = (names_length + size) * 2;
      char *grown = realloc(names, names_capacity);
      if (!grown) {
        errno = ENOMEM;
        result = -1;
        break;
      }
      names = grown;
    }
    memcpy(names + names_length, name, size);
    entries[count++] = (struct entry){entry->d_ino, names_length};
    names_length += size;
  }
  if (result == 0 && count >= RS_SORT_THRESHOLD && volume != IN_MEMORY) qsort(entries, count, sizeof *entries, by_inode);
  for (size_t i = 0; result == 0 && i < count; i++)
    result = visit(dirfd, names + entries[i].name, dev, volume, totals, depth, length);
  int saved = errno;
  free(entries);
  free(names);
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
  int root = dup(dirfd);
  if (root < 0) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return -1;
  }
  int result = walk(dirfd, info.st_dev, volume, totals, 0, 0);
  for (size_t i = 0; result == 0 && i < totals->linked_count; i++) {
    struct inode *entry = &totals->linked[i];
    if (entry->deferred && entry->seen >= entry->links)
      entry->private_size = private_bytes(root, entry->deferred, entry->private_size, &entry->shared);
  }
  int saved = errno;
  close(root);
  errno = saved;
  return result;
}

const char *measure_error(void) { return strerror(errno); }

int main(int argc, char **argv) { return real_size_main(argc, argv); }
