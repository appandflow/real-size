#ifndef REAL_SIZE_H
#define REAL_SIZE_H

#include <stddef.h>
#include <stdint.h>

#define MAX_DEPTH 256
#define MAX_DEPTH_TEXT "256"
#define UNSUPPORTED (-2)

struct inode {
  uint64_t dev;
  uint64_t ino;
  uint64_t ino_high;
  uint64_t private_size;
  uint32_t links;
  uint32_t seen;
  int shared;
  char *deferred;
};

struct totals {
  uint64_t allocated;
  uint64_t private_size;
  uint64_t files;
  uint64_t skipped;
  int exact;
  struct inode *linked;
  size_t linked_count;
  size_t *slots;
  size_t slot_capacity;
};

/* Counts one regular file. `shared` means part of `private_size` may be shared with another file, so the result is a
 * lower bound. A file with more than one link counts once per tree, and its private bytes count only when every link
 * was seen in the tree. Returns the file's entry when this call first recorded a file with more than one link, so the
 * caller can defer measuring its private bytes until the walk shows every link inside; NULL otherwise. */
struct inode *add_file(struct totals *totals, uint64_t dev, uint64_t ino, uint64_t ino_high, uint32_t links, uint64_t allocated,
              uint64_t private_size, int shared);

/* Walks the directory at the UTF-8 `path`. Returns 0, -1 with errno set, or UNSUPPORTED. -1 is for an error on `path`
 * itself or out of memory; an entry below it that cannot be read is counted with skip_entry and the walk goes on. */
int measure(const char *path, struct totals *totals);

/* The platform's message for the error `measure` returned -1 with. */
const char *measure_error(void);

/* Counts an entry the walk could not read. Returns 1 when the caller should name it with skip_message; past the
 * first few, only the count is kept. */
int skip_entry(struct totals *totals);
void skip_message(const char *path, const char *reason);

/* Returns 1 when the progress line is due for an update. Called at most once per directory. */
int progress_due(void);
void progress_show(const struct totals *totals, const char *directory);

/* The name REAL_SIZE_TEST_PAUSE_AT sets, or NULL. The walk calls test_pause before opening a directory of that name. */
extern const char *pause_at;
void test_pause(const char *path);

/* Prints "out of memory" and exits with status 1. */
void out_of_memory(void);

/* The worker pool in pool.c, shared by the platform walks. A platform's worker struct starts with its own totals; a
 * queued item is one malloc'ed block that the pool frees after `task` returns. */
typedef int (*pool_task)(void *worker, void *item);

/* REAL_SIZE_TEST_THREADS when set, for tests and benchmarks; otherwise the CPU count, at most 4. */
int pool_threads(void);

/* Starts a measurement whose files with several links go into `totals`' table. */
void pool_begin(struct totals *totals);
void pool_push(void *item);

/* When `result` is 0, runs `task` on queued items on `count` workers, `size` bytes apart, the calling thread being the
 * first, until the queue is empty or a task returns a negative value. Then adds every worker's totals into the
 * measurement's and returns `result`, or that negative value. */
int pool_run(void *workers, size_t size, int count, pool_task task, int result);

/* add_file for a worker: a file with one link counts in `own`, one with several in the shared table under its lock,
 * which stores a copy of `deferred` when not NULL in a newly recorded entry. Returns 1 when the file counted in
 * `files`. */
int count_file(struct totals *own, uint64_t dev, uint64_t ino, uint64_t ino_high, uint32_t links, uint64_t allocated,
               uint64_t private_size, int shared, const char *deferred);

/* Adds to the file count the progress line shows. */
void pool_seen(uint64_t files);

/* Serializes skip_entry, skip_message, test_pause and the progress line between workers. */
void output_begin(void);
void output_end(void);

/* Returns 1, holding the output lock, when the progress line is due and no other worker holds the lock; the caller
 * then calls progress_end with the directory to show. */
int progress_begin(void);
void progress_end(const char *directory);

/* A recursive lock that one thread at a time holds while it walks a directory too deep to queue by path. */
void long_path_begin(void);
void long_path_end(void);

/* Platform services for main.c. */
const char *environment(const char *name);
int stderr_is_terminal(void);
int terminal_width(void);
uint64_t monotonic_ms(void);

int real_size_main(int argc, char **argv);

#endif
