#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
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

/* A file with several links on btrfs or XFS is mapped only after the walk, and only when every link was inside the
 * tree; until then it is known by its path relative to the measured directory. */
struct worker {
  struct totals totals;
  char relative[PATH_MAX];
};

struct item {
  dev_t dev;
  enum volume volume;
  int crossed;
  int depth;
  size_t length;
  char relative[];
};

static const char *root_path;
static int root_fd;
static char shown[2 * PATH_MAX + NAME_MAX + 8];

/* The path of `name` in the directory `relative` holds `length` bytes of, or of that directory when `name` is NULL, as
 * the user wrote it. A directory whose relative path does not fit in `relative` is shown as `.../name`. Called with
 * the output lock held. */
static const char *entry_path(const char *relative, size_t length, const char *name) {
  size_t root_length = strlen(root_path);
  const char *separator = root_length && root_path[root_length - 1] == '/' ? "" : "/";
  int used;
  if (length == 0) used = snprintf(shown, sizeof shown, "%s", root_path);
  else if (length < PATH_MAX)
    used = snprintf(shown, sizeof shown, "%s%s%.*s", root_path, separator, (int)length - 1, relative + 1);
  else used = snprintf(shown, sizeof shown, "%s%s...", root_path, separator);
  if (name && used > 0 && (size_t)used < sizeof shown)
    snprintf(shown + used, sizeof shown - (size_t)used, "%s%s", shown[used - 1] == '/' ? "" : "/", name);
  return shown;
}

static void skip(struct worker *worker, size_t length, const char *name, int error, const char *reason) {
  if (error == ENOENT || error == ENOTDIR) return;
  output_begin();
  if (skip_entry(&worker->totals))
    skip_message(entry_path(worker->relative, length, name), reason ? reason : strerror(error));
  output_end();
}

/* Classifies a directory whose device differs from its parent's. Returns 0, or closes it, skips it as `name` in the
 * directory `length` describes and returns -1. */
static int enter_volume(struct worker *worker, int dirfd, size_t length, const char *name, enum volume *volume) {
  int classified = classify(dirfd, volume);
  if (classified == 0) return 0;
  int saved = errno;
  close(dirfd);
  if (classified == UNSUPPORTED) skip(worker, length, name, 0, "another filesystem that cannot report private size");
  else skip(worker, length, name, saved, NULL);
  return -1;
}

static int walk(struct worker *worker, int dirfd, dev_t dev, enum volume volume, int depth, size_t length);

static int walk_queued(void *argument, void *data) {
  struct worker *worker = argument;
  struct item *item = data;
  memcpy(worker->relative, item->relative, item->length + 1);
  int dirfd = openat(root_fd, worker->relative + 1, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0) {
    skip(worker, item->length, NULL, errno, NULL);
    return 0;
  }
  enum volume volume = item->volume;
  if (item->crossed && enter_volume(worker, dirfd, item->length, NULL, &volume) < 0) return 0;
  return walk(worker, dirfd, item->dev, volume, item->depth, item->length);
}

/* Subdirectories are queued by their path relative to the measured directory and opened from it by another worker.
 * One whose relative path does not fit in PATH_MAX is walked here from `dirfd` instead, one thread at a time, so the
 * descriptors held stay within one chain of MAX_DEPTH. */
static int walk(struct worker *worker, int dirfd, dev_t dev, enum volume volume, int depth, size_t length) {
  DIR *dir = fdopendir(dirfd);
  if (!dir) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    if (depth == 0) return -1;
    skip(worker, length, NULL, saved, NULL);
    return 0;
  }
  if (progress_begin()) progress_end(entry_path(worker->relative, length, NULL));
  char *relative = worker->relative;
  uint64_t files = 0;
  int result = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno && depth == 0) result = -1;
      else if (errno) skip(worker, length, NULL, errno, NULL);
      break;
    }
    const char *name = entry->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR && entry->d_type != DT_REG) continue;
    size_t name_length = strlen(name);
    size_t child_length = length + name_length + 1;
    if (child_length < PATH_MAX) {
      relative[length] = '/';
      memcpy(relative + length + 1, name, name_length + 1);
    }
    struct stat info;
    if (fstatat(dirfd, name, &info, AT_SYMLINK_NOFOLLOW) < 0) {
      skip(worker, length, name, errno, NULL);
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      if (depth + 1 >= MAX_DEPTH) {
        skip(worker, length, name, ELOOP, "more than " MAX_DEPTH_TEXT " nested directories");
        continue;
      }
      if (pause_at && strcmp(name, pause_at) == 0) {
        output_begin();
        test_pause(entry_path(relative, length, name));
        output_end();
      }
      if (child_length < PATH_MAX) {
        struct item *item = malloc(sizeof *item + child_length + 1);
        if (!item) out_of_memory();
        item->dev = info.st_dev;
        item->volume = volume;
        item->crossed = info.st_dev != dev;
        item->depth = depth + 1;
        item->length = child_length;
        memcpy(item->relative, relative, child_length + 1);
        pool_push(item);
        continue;
      }
      long_path_begin();
      int child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      enum volume child_volume = volume;
      if (child < 0) skip(worker, length, name, errno, NULL);
      else if (info.st_dev == dev || enter_volume(worker, child, length, name, &child_volume) == 0)
        result = walk(worker, child, info.st_dev, child_volume, depth + 1, child_length);
      long_path_end();
      if (result < 0) break;
    } else if (S_ISREG(info.st_mode)) {
      uint64_t allocated = (uint64_t)info.st_blocks * 512;
      uint64_t private_size = allocated;
      int shared = 0;
      int defer = volume == EXTENT_MAPPED && allocated > 0 && info.st_nlink > 1 && child_length < PATH_MAX;
      if (volume == EXTENT_MAPPED && allocated > 0 && !defer)
        private_size = private_bytes(dirfd, name, allocated, &shared);
      files += (uint64_t)count_file(&worker->totals, (uint64_t)info.st_dev, (uint64_t)info.st_ino, 0,
                                    (uint32_t)info.st_nlink, allocated, private_size, shared, defer ? relative + 1 : NULL);
    }
  }
  pool_seen(files);
  int saved = errno;
  closedir(dir);
  errno = saved;
  return result;
}

int measure(const char *path, struct totals *totals) {
  int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0) return -1;
  root_path = path;
  struct stat info;
  enum volume volume = LINK_COUNTED;
  int classified = fstat(dirfd, &info) < 0 ? -1 : classify(dirfd, &volume);
  if (classified < 0) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return classified;
  }
  root_fd = dup(dirfd);
  if (root_fd < 0) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return -1;
  }
  int count = pool_threads();
  struct worker *workers = calloc((size_t)count, sizeof *workers);
  if (!workers) out_of_memory();
  for (int i = 0; i < count; i++) workers[i].totals.exact = 1;
  pool_begin(totals);
  int result = walk(&workers[0], dirfd, info.st_dev, volume, 0, 0);
  int saved = errno;
  result = pool_run(workers, sizeof *workers, count, walk_queued, result);
  free(workers);
  for (size_t i = 0; result == 0 && i < totals->linked_count; i++) {
    struct inode *entry = &totals->linked[i];
    if (entry->deferred && entry->seen >= entry->links)
      entry->private_size = private_bytes(root_fd, entry->deferred, entry->private_size, &entry->shared);
  }
  close(root_fd);
  errno = saved;
  return result;
}

const char *measure_error(void) { return strerror(errno); }

int main(int argc, char **argv) { return real_size_main(argc, argv); }
