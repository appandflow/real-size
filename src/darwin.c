#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <unistd.h>

#include "real-size.h"

#define BUFFER_SIZE (64 * 1024)

static char *buffers;

/* APFS reports the bytes a file does not share with a clone as ATTR_CMNEXT_PRIVATESIZE (sys/attr.h), requested with
 * FSOPT_ATTR_CMN_EXTENDED. Other volume formats omit it. getattrlistbulk descends into volumes mounted inside the tree;
 * symbolic links are listed, not followed. */
static int walk(int dirfd, struct totals *totals, char *buffer, int depth) {
  struct attrlist request = {0};
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.commonattr =
      ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
  request.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE;
  request.forkattr = ATTR_CMNEXT_PRIVATESIZE;
  for (;;) {
    int count = getattrlistbulk(dirfd, &request, buffer, BUFFER_SIZE, FSOPT_ATTR_CMN_EXTENDED);
    if (count < 0) return -1;
    if (count == 0) return 0;
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
      attrreference_t name_ref;
      memcpy(&name_ref, field, sizeof name_ref);
      char *name = field + name_ref.attr_dataoffset;
      field += sizeof(attrreference_t);
      dev_t dev;
      memcpy(&dev, field, sizeof dev);
      field += sizeof(dev_t);
      fsobj_type_t type;
      memcpy(&type, field, sizeof type);
      field += sizeof(fsobj_type_t);
      uint64_t id;
      memcpy(&id, field, sizeof id);
      field += sizeof(uint64_t);
      if (error == 0 && type == VDIR) {
        if (depth + 1 >= MAX_DEPTH) {
          errno = ELOOP;
          return -1;
        }
        int child = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) return -1;
        int walked = walk(child, totals, buffer + BUFFER_SIZE, depth + 1);
        close(child);
        if (walked < 0) return walked;
      } else if (error == 0 && type == VREG) {
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
        add_file(totals, (uint64_t)dev, id, 0, links, (uint64_t)allocated, (uint64_t)private_size,
                 private_size < allocated);
      }
      entry += length;
    }
  }
}

int measure(const char *path, struct totals *totals) {
  if (!buffers) {
    buffers = malloc((size_t)BUFFER_SIZE * MAX_DEPTH);
    if (!buffers) {
      errno = ENOMEM;
      return -1;
    }
  }
  int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dirfd < 0) return -1;
  int walked = walk(dirfd, totals, buffers, 0);
  int saved = errno;
  close(dirfd);
  errno = saved;
  return walked;
}

const char *measure_error(void) { return strerror(errno); }

int main(int argc, char **argv) { return real_size_main(argc, argv); }
