#ifndef REAL_SIZE_H
#define REAL_SIZE_H

#include <stddef.h>
#include <stdint.h>

#define MAX_DEPTH 256
#define UNSUPPORTED (-2)

struct inode {
  uint64_t dev;
  uint64_t ino;
  uint64_t ino_high;
  uint64_t private_size;
  uint32_t links;
  uint32_t seen;
  int shared;
};

struct totals {
  uint64_t allocated;
  uint64_t private_size;
  uint64_t files;
  int exact;
  struct inode *linked;
  size_t linked_count;
  size_t *slots;
  size_t slot_capacity;
};

/* Counts one regular file. `shared` means part of `private_size` may be shared with another file, so the result is a
 * lower bound. A file with more than one link counts once per tree, and its private bytes count only when every link
 * was seen in the tree. */
void add_file(struct totals *totals, uint64_t dev, uint64_t ino, uint64_t ino_high, uint32_t links, uint64_t allocated,
              uint64_t private_size, int shared);

/* Walks the directory at the UTF-8 `path`. Returns 0, -1 with errno set, or UNSUPPORTED. */
int measure(const char *path, struct totals *totals);

/* The platform's message for the error `measure` returned -1 with. */
const char *measure_error(void);

int real_size_main(int argc, char **argv);

#endif
