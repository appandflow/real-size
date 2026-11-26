#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <unistd.h>

#include "real-size.h"

#define BUFFER_SIZE (64 * 1024)
#define MAX_THREADS 4

struct item {
  char *path;
  dev_t parent;
  int depth;
};

struct worker {
  pthread_t thread;
  struct totals totals;
  char *buffer;
};

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t long_path_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
static struct item *queue;
static size_t queued, queue_capacity, busy;
static int stop_result;
static struct totals *linked_totals;
static size_t root_expansion;
static atomic_uint_fast64_t files_seen;

static void out_of_memory(void) {
  fputs("real-size: out of memory\n", stderr);
  exit(1);
}

static char *join(const char *parent, const char *name) {
  size_t parent_length = strlen(parent), name_length = strlen(name);
  size_t separator = parent_length > 0 && parent[parent_length - 1] != '/';
  char *path = malloc(parent_length + separator + name_length + 1);
  if (!path) out_of_memory();
  memcpy(path, parent, parent_length);
  if (separator) path[parent_length] = '/';
  memcpy(path + parent_length + separator, name, name_length + 1);
  return path;
}

static void skip(struct totals *totals, const char *path, const char *name, int error, const char *reason) {
  if (error == ENOENT || error == ENOTDIR) return;
  pthread_mutex_lock(&output_lock);
  if (skip_entry(totals)) {
    char *shown = name ? join(path, name) : NULL;
    skip_message(shown ? shown : path, reason ? reason : strerror(error));
    free(shown);
  }
  pthread_mutex_unlock(&output_lock);
}

static void push(char *path, dev_t parent, int depth) {
  pthread_mutex_lock(&queue_lock);
  if (queued == queue_capacity) {
    queue_capacity = queue_capacity ? queue_capacity * 2 : 256;
    struct item *grown = realloc(queue, queue_capacity * sizeof *queue);
    if (!grown) out_of_memory();
    queue = grown;
  }
  queue[queued++] = (struct item){path, parent, depth};
  pthread_cond_signal(&queue_changed);
  pthread_mutex_unlock(&queue_lock);
}

static void show_progress(const char *path) {
  if (pthread_mutex_trylock(&output_lock) != 0) return;
  if (progress_due()) {
    struct totals view = {0};
    view.files = atomic_load(&files_seen);
    progress_show(&view, path);
  }
  pthread_mutex_unlock(&output_lock);
}

/* APFS reports the bytes a file does not share with a clone as ATTR_CMNEXT_PRIVATESIZE (sys/attr.h), requested with
 * FSOPT_ATTR_CMN_EXTENDED. Other volume formats omit it. */
static int reports_private_size(int fd) {
  struct statfs info;
  if (fstatfs(fd, &info) < 0) return -1;
  return strcmp(info.f_fstypename, "apfs") == 0;
}

/* getattrlistbulk lists a mount point with the device of the directory it covers, so a volume mounted inside the tree
 * shows as a change of device in its own listing, before anything in it is counted. A volume whose root lists nothing
 * is therefore not reported as skipped; it holds nothing to count. Symbolic links are listed, not followed.
 * Subdirectories are queued by path; one whose path does not fit in PATH_MAX once symbolic links in the measured path
 * are expanded, which open(2) rejects, is walked here from `dirfd` instead, one thread at a time so the descriptors
 * held stay within one chain of MAX_DEPTH. */
static int walk(int dirfd, dev_t parent, const char *path, struct totals *totals, char *buffer, int depth) {
  show_progress(path);
  struct attrlist request = {0};
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.commonattr =
      ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
  request.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE;
  request.forkattr = ATTR_CMNEXT_PRIVATESIZE;
  for (;;) {
    int count = getattrlistbulk(dirfd, &request, buffer, BUFFER_SIZE, FSOPT_ATTR_CMN_EXTENDED);
    if (count < 0) {
      if (depth == 0) return -1;
      skip(totals, path, NULL, errno, NULL);
      return 0;
    }
    if (count == 0) return 0;
    uint64_t files = 0;
    char *entry = buffer;
    for (int i = 0; i < count; i++) {
      char *field = entry;
      uint32_t length;
      memcpy(&length, field, sizeof length);
      field += sizeof(uint32_t);
      attribute_set_t returned;
      memcpy(&returned, field, sizeof returned);
      field += sizeof(attribute_set_t);
      uint32_t error = 0;
      if (returned.commonattr & ATTR_CMN_ERROR) {
        memcpy(&error, field, sizeof error);
        field += sizeof(uint32_t);
      }
      char *name = NULL;
      if (returned.commonattr & ATTR_CMN_NAME) {
        attrreference_t name_ref;
        memcpy(&name_ref, field, sizeof name_ref);
        name = field + name_ref.attr_dataoffset;
        field += sizeof(attrreference_t);
      }
      if (error != 0 || !name) {
        skip(totals, path, name, error ? (int)error : EIO, NULL);
        entry += length;
        continue;
      }
      dev_t dev;
      memcpy(&dev, field, sizeof dev);
      field += sizeof(dev_t);
      if (dev != parent) {
        int reports = reports_private_size(dirfd);
        if (reports <= 0) {
          if (reports < 0) skip(totals, path, NULL, errno, NULL);
          else skip(totals, path, NULL, 0, "another filesystem that cannot report private size");
          return 0;
        }
        parent = dev;
      }
      fsobj_type_t type;
      memcpy(&type, field, sizeof type);
      field += sizeof(fsobj_type_t);
      uint64_t id;
      memcpy(&id, field, sizeof id);
      field += sizeof(uint64_t);
      if (type == VDIR) {
        char *child_path = join(path, name);
        if (pause_at && strcmp(name, pause_at) == 0) {
          pthread_mutex_lock(&output_lock);
          test_pause(child_path);
          pthread_mutex_unlock(&output_lock);
        }
        int child = -1;
        if (depth + 1 >= MAX_DEPTH) {
          skip(totals, path, name, ELOOP, "more than " MAX_DEPTH_TEXT " nested directories");
          free(child_path);
        } else if (strlen(child_path) + root_expansion < PATH_MAX) push(child_path, dev, depth + 1);
        else {
          int walked = 0;
          pthread_mutex_lock(&long_path_lock);
          if ((child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
            skip(totals, path, name, errno, NULL);
          else {
            char *child_buffer = malloc(BUFFER_SIZE);
            if (!child_buffer) out_of_memory();
            walked = walk(child, dev, child_path, totals, child_buffer, depth + 1);
            free(child_buffer);
            close(child);
          }
          pthread_mutex_unlock(&long_path_lock);
          free(child_path);
          if (walked < 0) return walked;
        }
      } else if (type == VREG) {
        uint32_t links = 1;
        off_t allocated = 0, private_size = 0;
        if (returned.fileattr & ATTR_FILE_LINKCOUNT) {
          memcpy(&links, field, sizeof links);
          field += sizeof(uint32_t);
        }
        if (returned.fileattr & ATTR_FILE_ALLOCSIZE) {
          memcpy(&allocated, field, sizeof allocated);
          field += sizeof(off_t);
        }
        if (!(returned.forkattr & ATTR_CMNEXT_PRIVATESIZE)) return UNSUPPORTED;
        memcpy(&private_size, field, sizeof private_size);
        files++;
        if (links <= 1) add_file(totals, (uint64_t)dev, id, 0, links, (uint64_t)allocated, (uint64_t)private_size,
                                 private_size < allocated);
        else {
          pthread_mutex_lock(&table_lock);
          add_file(linked_totals, (uint64_t)dev, id, 0, links, (uint64_t)allocated, (uint64_t)private_size,
                   private_size < allocated);
          pthread_mutex_unlock(&table_lock);
        }
      }
      entry += length;
    }
    atomic_fetch_add(&files_seen, files);
  }
}

static void *work(void *argument) {
  struct worker *worker = argument;
  for (;;) {
    pthread_mutex_lock(&queue_lock);
    while (queued == 0 && busy > 0 && stop_result == 0) pthread_cond_wait(&queue_changed, &queue_lock);
    if (queued == 0 || stop_result != 0) {
      pthread_mutex_unlock(&queue_lock);
      return NULL;
    }
    struct item item = queue[--queued];
    busy++;
    pthread_mutex_unlock(&queue_lock);
    int walked = 0;
    int dirfd = open(item.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) skip(&worker->totals, item.path, NULL, errno, NULL);
    else {
      walked = walk(dirfd, item.parent, item.path, &worker->totals, worker->buffer, item.depth);
      close(dirfd);
    }
    free(item.path);
    pthread_mutex_lock(&queue_lock);
    busy--;
    if (walked < 0 && stop_result == 0) stop_result = walked;
    if (stop_result != 0 || (queued == 0 && busy == 0)) pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
  }
}

static int thread_count(void) {
  const char *configured = environment("REAL_SIZE_TEST_THREADS");
  long count = configured ? strtol(configured, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
  if (!configured && count > MAX_THREADS) count = MAX_THREADS;
  return count < 1 ? 1 : (int)count;
}

int measure(const char *path, struct totals *totals) {
  int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0) return -1;
  struct stat info;
  int reports = fstat(dirfd, &info) < 0 ? -1 : reports_private_size(dirfd);
  if (reports <= 0) {
    int saved = errno;
    close(dirfd);
    errno = saved;
    return reports < 0 ? -1 : UNSUPPORTED;
  }
  char resolved[PATH_MAX];
  size_t length = strlen(path);
  if (fcntl(dirfd, F_GETPATH, resolved) < 0) root_expansion = PATH_MAX;
  else root_expansion = strlen(resolved) > length ? strlen(resolved) - length : 0;
  int count = thread_count();
  struct worker *workers = calloc((size_t)count, sizeof *workers);
  if (!workers) out_of_memory();
  for (int i = 0; i < count; i++) {
    workers[i].totals.exact = 1;
    workers[i].buffer = malloc(BUFFER_SIZE);
    if (!workers[i].buffer) out_of_memory();
  }
  linked_totals = totals;
  queued = busy = 0;
  stop_result = 0;
  atomic_store(&files_seen, 0);
  int walked = walk(dirfd, info.st_dev, path, &workers[0].totals, workers[0].buffer, 0);
  int saved = errno;
  close(dirfd);
  if (walked == 0) {
    int started = 1;
    for (; started < count; started++)
      if (pthread_create(&workers[started].thread, NULL, work, &workers[started]) != 0) break;
    work(&workers[0]);
    for (int i = 1; i < started; i++) pthread_join(workers[i].thread, NULL);
    walked = stop_result;
  }
  while (queued > 0) free(queue[--queued].path);
  for (int i = 0; i < count; i++) {
    totals->allocated += workers[i].totals.allocated;
    totals->private_size += workers[i].totals.private_size;
    totals->files += workers[i].totals.files;
    totals->skipped += workers[i].totals.skipped;
    totals->exact &= workers[i].totals.exact;
    free(workers[i].buffer);
  }
  free(workers);
  errno = saved;
  return walked;
}

const char *measure_error(void) { return strerror(errno); }

int main(int argc, char **argv) { return real_size_main(argc, argv); }
