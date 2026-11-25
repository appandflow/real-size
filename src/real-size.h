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

/* Platform services for main.c. */
const char *environment(const char *name);
int stderr_is_terminal(void);
int terminal_width(void);
uint64_t monotonic_ms(void);

int real_size_main(int argc, char **argv);

#endif
