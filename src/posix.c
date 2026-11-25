#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "real-size.h"

const char *environment(const char *name) { return getenv(name); }

int stderr_is_terminal(void) { return isatty(STDERR_FILENO); }

int terminal_width(void) {
  struct winsize size;
  return ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 ? size.ws_col : 0;
}

uint64_t monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
