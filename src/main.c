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
#ifdef REAL_SIZE_TARGET
#define TARGET_STRING STRINGIFY(REAL_SIZE_TARGET)
#else
#define TARGET_STRING "unknown"
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

#define SKIP_LINES 10

static uint64_t skips_named, skips_unnamed;

int skip_entry(struct totals *totals) {
  totals->skipped++;
  if (skips_named < SKIP_LINES) {
    skips_named++;
    return 1;
  }
  skips_unnamed++;
  return 0;
}

static void progress_clear(void);

void skip_message(const char *path, const char *reason) {
  progress_clear();
  fprintf(stderr, "real-size: %s: %s\n", path, reason);
}

const char *pause_at;

void test_pause(const char *path) {
  progress_clear();
  fprintf(stderr, "real-size: test pause at %s\n", path);
  for (int c = getchar(); c != EOF && c != '\n'; c = getchar()) {
  }
}

static int progress_on;
static uint64_t progress_next, progress_base;
static size_t progress_shown;
static char progress_line[1024];

int progress_due(void) {
  if (!progress_on) return 0;
  uint64_t now = monotonic_ms();
  if (now < progress_next) return 0;
  progress_next = now + 100;
  return 1;
}

static size_t columns(const char *text, size_t bytes) {
  size_t count = 0;
  for (size_t i = 0; i < bytes; i++) count += ((unsigned char)text[i] & 0xc0) != 0x80;
  return count;
}

/* Overwrites the previous line, padding with spaces rather than an erase sequence, which a Windows console without
 * virtual terminal processing would print. The width is in code points, so wide characters can still wrap. */
void progress_show(const struct totals *totals, const char *directory) {
  size_t width = (size_t)terminal_width();
  if (width < 20) width = 80;
  if (width > sizeof progress_line / 2) width = sizeof progress_line / 2;
  width--;
  int prefix = snprintf(progress_line, sizeof progress_line, "\r%llu files, ",
                        (unsigned long long)(progress_base + totals->files));
  if (prefix < 0) return;
  size_t used = (size_t)prefix, room = width + 1 > used ? width + 1 - used : 0;
  size_t length = strlen(directory);
  if (columns(directory, length) > room && room > 3) {
    while (*directory && columns(directory, length) > room - 3) {
      directory++;
      while (((unsigned char)*directory & 0xc0) == 0x80) directory++;
      length = strlen(directory);
    }
    memcpy(progress_line + used, "...", 3);
    used += 3;
  } else if (columns(directory, length) > room)
    length = 0;
  memcpy(progress_line + used, directory, length);
  used += length;
  size_t shown = columns(progress_line, used) - 1;
  for (size_t i = shown; i < progress_shown && used < sizeof progress_line; i++) progress_line[used++] = ' ';
  progress_shown = shown;
  fwrite(progress_line, 1, used, stderr);
}

static void progress_clear(void) {
  if (!progress_shown) return;
  size_t used = 0;
  progress_line[used++] = '\r';
  for (size_t i = 0; i < progress_shown && used < sizeof progress_line - 1; i++) progress_line[used++] = ' ';
  progress_line[used++] = '\r';
  fwrite(progress_line, 1, used, stderr);
  progress_shown = 0;
}

struct result {
  const char *path;
  uint64_t allocated;
  uint64_t private_size;
  uint64_t files;
  uint64_t skipped;
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
  printf(" allocated, %llu files", (unsigned long long)result->files);
  if (result->skipped) printf(" (%llu entries could not be read)", (unsigned long long)result->skipped);
  putchar('\n');
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
  printf("\"allocated\":%llu,\"private\":%llu,\"files\":%llu,\"exact\":%s,\"skipped\":%llu",
         (unsigned long long)result->allocated, (unsigned long long)result->private_size,
         (unsigned long long)result->files, result->exact ? "true" : "false", (unsigned long long)result->skipped);
}

static const char usage[] = "usage: real-size [--json] [--no-progress] <path>...\n";

int real_size_main(int argc, char **argv) {
  int json = 0, progress = 1, options = 1, count = 0;
  const char **paths = calloc((size_t)argc + 1, sizeof *paths);
  if (!paths) out_of_memory();
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (options && strcmp(arg, "--") == 0) options = 0;
    else if (options && strcmp(arg, "--json") == 0) json = 1;
    else if (options && strcmp(arg, "--no-progress") == 0) progress = 0;
    else if (options && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
      fputs(usage, stdout);
      puts("\nReports the bytes the regular files under each directory occupy on disk (allocated)\n"
           "and the bytes deleting that directory would free (private). Clones, hard links to\n"
           "files outside the tree and shared extents do not count as private.\n"
           "\n"
           "Entries that cannot be read are skipped and named on stderr; the sizes are\n"
           "then lower bounds. On a terminal, a walk that takes more than two seconds\n"
           "shows its progress on stderr.\n"
           "\n"
           "  --json         print one JSON document\n"
           "  --no-progress  never show progress\n"
           "  --version      print the version\n"
           "\n"
           "Exit status: 0 when every path was measured in full; 1 on a usage error, when\n"
           "a path cannot be read (nothing printed), or when entries below a path were\n"
           "skipped (sizes printed); 2 when the filesystem cannot report the bytes deleting\n"
           "a path would free.");
      return 0;
    } else if (options && strcmp(arg, "--version") == 0) {
      puts(VERSION_STRING " (" TARGET_STRING ")");
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
  struct result total = {"total", 0, 0, 0, 0, 1};
  pause_at = environment("REAL_SIZE_TEST_PAUSE_AT");
  progress_on = progress && stderr_is_terminal();
  const char *after = environment("REAL_SIZE_TEST_PROGRESS_AFTER_MS");
  progress_next = monotonic_ms() + (after ? strtoull(after, NULL, 10) : 2000);
  for (int i = 0; i < count; i++) {
    struct totals totals = {0};
    totals.exact = 1;
    progress_base = total.files;
    int measured = measure(paths[i], &totals);
    settle_links(&totals);
    if (measured == UNSUPPORTED) {
      progress_clear();
      fprintf(stderr, "real-size: %s: the filesystem cannot report the bytes deleting it would free\n", paths[i]);
      return 2;
    }
    if (measured < 0) {
      const char *message = measure_error();
      progress_clear();
      fprintf(stderr, "real-size: %s: %s\n", paths[i], message);
      return 1;
    }
    if (totals.skipped) totals.exact = 0;
    results[i] =
        (struct result){paths[i], totals.allocated, totals.private_size, totals.files, totals.skipped, totals.exact};
    total.allocated += totals.allocated;
    total.private_size += totals.private_size;
    total.files += totals.files;
    total.skipped += totals.skipped;
    total.exact &= totals.exact;
  }
  progress_clear();
  if (skips_unnamed)
    fprintf(stderr, "real-size: and %llu more entries could not be read\n", (unsigned long long)skips_unnamed);
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
  if (fflush(stdout) != 0) return 1;
  return total.skipped ? 1 : 0;
}
