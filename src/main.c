#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real-size.h"

#define STRINGIFY_(value) #value
#define STRINGIFY(value) STRINGIFY_(value)
#ifdef REAL_SIZE_VERSION
#define VERSION_STRING STRINGIFY(REAL_SIZE_VERSION)
#else
#define VERSION_STRING "dev"
#endif

static void out_of_memory(void) {
  fputs("real-size: out of memory\n", stderr);
  exit(1);
}

static size_t slot_of(const struct totals *totals, uint64_t dev, uint64_t ino, uint64_t ino_high) {
  uint64_t hash = (ino ^ (ino_high * 0xc2b2ae3d27d4eb4fULL) ^ (dev * 0x9e3779b97f4a7c15ULL)) * 0xff51afd7ed558ccdULL;
  size_t mask = totals->slot_capacity - 1;
  size_t slot = (size_t)(hash >> 17) & mask;
  while (totals->slots[slot]) {
    const struct inode *entry = &totals->linked[totals->slots[slot] - 1];
    if (entry->dev == dev && entry->ino == ino && entry->ino_high == ino_high) break;
    slot = (slot + 1) & mask;
  }
  return slot;
}

static void grow_slots(struct totals *totals) {
  free(totals->slots);
  totals->slot_capacity = totals->slot_capacity ? totals->slot_capacity * 2 : 1024;
  totals->slots = calloc(totals->slot_capacity, sizeof *totals->slots);
  if (!totals->slots) out_of_memory();
  for (size_t i = 0; i < totals->linked_count; i++) {
    const struct inode *entry = &totals->linked[i];
    totals->slots[slot_of(totals, entry->dev, entry->ino, entry->ino_high)] = i + 1;
  }
}

struct inode *add_file(struct totals *totals, uint64_t dev, uint64_t ino, uint64_t ino_high, uint32_t links, uint64_t allocated,
              uint64_t private_size, int shared) {
  if (links <= 1) {
    totals->allocated += allocated;
    totals->files++;
    totals->private_size += private_size;
    if (shared) totals->exact = 0;
    return NULL;
  }
  if ((totals->linked_count + 1) * 2 > totals->slot_capacity) grow_slots(totals);
  size_t slot = slot_of(totals, dev, ino, ino_high);
  if (totals->slots[slot]) {
    totals->linked[totals->slots[slot] - 1].seen++;
    return NULL;
  }
  if ((totals->linked_count & (totals->linked_count - 1)) == 0) {
    size_t capacity = totals->linked_count ? totals->linked_count * 2 : 1;
    struct inode *grown = realloc(totals->linked, capacity * sizeof *totals->linked);
    if (!grown) out_of_memory();
    totals->linked = grown;
  }
  totals->linked[totals->linked_count] = (struct inode){dev, ino, ino_high, private_size, links, 1, shared, NULL};
  totals->slots[slot] = ++totals->linked_count;
  totals->allocated += allocated;
  totals->files++;
  return &totals->linked[totals->linked_count - 1];
}

static void settle_links(struct totals *totals) {
  for (size_t i = 0; i < totals->linked_count; i++) {
    const struct inode *entry = &totals->linked[i];
    free(entry->deferred);
    if (entry->seen < entry->links) continue;
    totals->private_size += entry->private_size;
    if (entry->shared) totals->exact = 0;
  }
  free(totals->linked);
  free(totals->slots);
  totals->linked = NULL;
  totals->slots = NULL;
  totals->linked_count = 0;
  totals->slot_capacity = 0;
}

struct result {
  const char *path;
  uint64_t allocated;
  uint64_t private_size;
  uint64_t files;
  int exact;
};

static void print_size(uint64_t bytes) {
  static const char *units[] = {"B", "kB", "MB", "GB", "TB", "PB"};
  if (bytes < 1000) {
    printf("%llu B", (unsigned long long)bytes);
    return;
  }
  double value = (double)bytes;
  int unit = 0;
  while (value >= 999.95 && unit < 5) {
    value /= 1000;
    unit++;
  }
  printf("%.1f %s", value, units[unit]);
}

static void print_plain(const char *label, const struct result *result) {
  printf("%s: ", label);
  if (!result->exact) fputs("at least ", stdout);
  print_size(result->private_size);
  fputs(" private, ", stdout);
  print_size(result->allocated);
  printf(" allocated, %llu files\n", (unsigned long long)result->files);
}

static void print_json_string(const char *text) {
  putchar('"');
  for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
    if (*c == '"' || *c == '\\') printf("\\%c", *c);
    else if (*c < 0x20) printf("\\u%04x", *c);
    else putchar(*c);
  }
  putchar('"');
}

static void print_json_fields(const struct result *result) {
  printf("\"allocated\":%llu,\"private\":%llu,\"files\":%llu,\"exact\":%s", (unsigned long long)result->allocated,
         (unsigned long long)result->private_size, (unsigned long long)result->files,
         result->exact ? "true" : "false");
}

static const char usage[] = "usage: real-size [--json] <path>...\n";

int real_size_main(int argc, char **argv) {
  int json = 0, options = 1, count = 0;
  const char **paths = calloc((size_t)argc + 1, sizeof *paths);
  if (!paths) out_of_memory();
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && strcmp(arg, "--") == 0) options = 0;
    else if (options && strcmp(arg, "--json") == 0) json = 1;
    else if (options && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
      fputs(usage, stdout);
      puts("\nReports the bytes the regular files under each directory occupy on disk (allocated)\n"
           "and the bytes deleting that directory would free (private). Clones, hard links to\n"
           "files outside the tree and shared extents do not count as private.\n"
           "\n"
           "  --json     print one JSON document\n"
           "  --version  print the version\n"
           "\n"
           "Exit status: 0 on success, 1 on a usage or I/O error, 2 when the filesystem\n"
           "cannot report the bytes deleting a path would free.");
      return 0;
    } else if (options && strcmp(arg, "--version") == 0) {
      puts(VERSION_STRING);
      return 0;
    } else if (options && arg[0] == '-' && arg[1] != 0) {
      fprintf(stderr, "real-size: unknown option %s\n%s", arg, usage);
      return 1;
    } else paths[count++] = arg;
  }
  if (count == 0) {
    fputs(usage, stderr);
    return 1;
  }
  struct result *results = calloc((size_t)count, sizeof *results);
  if (!results) out_of_memory();
  struct result total = {"total", 0, 0, 0, 1};
  for (int i = 0; i < count; i++) {
    struct totals totals = {0};
    totals.exact = 1;
    int measured = measure(paths[i], &totals);
    settle_links(&totals);
    if (measured == UNSUPPORTED) {
      fprintf(stderr, "real-size: %s: the filesystem cannot report the bytes deleting it would free\n", paths[i]);
      return 2;
    }
    if (measured < 0) {
      fprintf(stderr, "real-size: %s: %s\n", paths[i], measure_error());
      return 1;
    }
    results[i] = (struct result){paths[i], totals.allocated, totals.private_size, totals.files, totals.exact};
    total.allocated += totals.allocated;
    total.private_size += totals.private_size;
    total.files += totals.files;
    total.exact &= totals.exact;
  }
  if (json) {
    fputs("{\"paths\":[", stdout);
    for (int i = 0; i < count; i++) {
      fputs(i ? ",{\"path\":" : "{\"path\":", stdout);
      print_json_string(results[i].path);
      putchar(',');
      print_json_fields(&results[i]);
      putchar('}');
    }
    fputs("],\"total\":{", stdout);
    print_json_fields(&total);
    fputs("}}\n", stdout);
  } else {
    for (int i = 0; i < count; i++) print_plain(results[i].path, &results[i]);
    if (count > 1) print_plain("total", &total);
  }
  return fflush(stdout) == 0 ? 0 : 1;
}
