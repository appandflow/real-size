#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "real-size.h"

#define MAX_THREADS 4

#ifdef _WIN32
typedef SRWLOCK lock_t;
#define LOCK_INIT SRWLOCK_INIT
#define lock(l) AcquireSRWLockExclusive(l)
#define unlock(l) ReleaseSRWLockExclusive(l)
#define try_lock(l) TryAcquireSRWLockExclusive(l)
static CONDITION_VARIABLE queue_changed = CONDITION_VARIABLE_INIT;
#define wait_queue(l) SleepConditionVariableSRW(&queue_changed, l, INFINITE, 0)
#define signal_queue() WakeConditionVariable(&queue_changed)
#define broadcast_queue() WakeAllConditionVariable(&queue_changed)
static volatile LONG64 files_seen;
#define add_seen(count) InterlockedExchangeAdd64(&files_seen, (LONG64)(count))
#define load_seen() ((uint64_t)InterlockedCompareExchange64(&files_seen, 0, 0))
#define store_seen(value) InterlockedExchange64(&files_seen, (LONG64)(value))
#else
typedef pthread_mutex_t lock_t;
#define LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
#define try_lock(l) (pthread_mutex_trylock(l) == 0)
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
#define wait_queue(l) pthread_cond_wait(&queue_changed, l)
#define signal_queue() pthread_cond_signal(&queue_changed)
#define broadcast_queue() pthread_cond_broadcast(&queue_changed)
static uint64_t files_seen;
#define add_seen(count) __atomic_fetch_add(&files_seen, (count), __ATOMIC_RELAXED)
#define load_seen() __atomic_load_n(&files_seen, __ATOMIC_RELAXED)
#define store_seen(value) __atomic_store_n(&files_seen, (value), __ATOMIC_RELAXED)
#endif

static lock_t queue_lock = LOCK_INIT, table_lock = LOCK_INIT, output_lock = LOCK_INIT;
static void **queue;
static size_t queued, queue_capacity, busy;
static int stop_result;
static struct totals *shared_totals;

int pool_threads(void) {
  const char *configured = environment("REAL_SIZE_TEST_THREADS");
  if (configured && *configured) {
    long count = strtol(configured, NULL, 10);
    return count < 1 ? 1 : count > 1024 ? 1024 : (int)count;
  }
#ifdef _WIN32
  long count = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
  long count = sysconf(_SC_NPROCESSORS_ONLN);
#endif
  return count < 1 ? 1 : count > MAX_THREADS ? MAX_THREADS : (int)count;
}

void pool_begin(struct totals *totals) {
  shared_totals = totals;
  queued = busy = 0;
  stop_result = 0;
  store_seen(0);
}

void pool_push(void *item) {
  lock(&queue_lock);
  if (queued == queue_capacity) {
    queue_capacity = queue_capacity ? queue_capacity * 2 : 256;
    void **grown = realloc(queue, queue_capacity * sizeof *queue);
    if (!grown) out_of_memory();
    queue = grown;
  }
  queue[queued++] = item;
  signal_queue();
  unlock(&queue_lock);
}

struct runner {
  void *worker;
  pool_task task;
};

static void work(void *worker, pool_task task) {
  for (;;) {
    lock(&queue_lock);
    while (queued == 0 && busy > 0 && stop_result == 0) wait_queue(&queue_lock);
    if (queued == 0 || stop_result != 0) {
      unlock(&queue_lock);
      return;
    }
    void *item = queue[--queued];
    busy++;
    unlock(&queue_lock);
    int result = task(worker, item);
    free(item);
    lock(&queue_lock);
    busy--;
    if (result < 0 && stop_result == 0) stop_result = result;
    if (stop_result != 0 || (queued == 0 && busy == 0)) broadcast_queue();
    unlock(&queue_lock);
  }
}

#ifdef _WIN32
static DWORD WINAPI start(LPVOID argument) {
  struct runner *runner = argument;
  work(runner->worker, runner->task);
  return 0;
}
#else
static void *start(void *argument) {
  struct runner *runner = argument;
  work(runner->worker, runner->task);
  return NULL;
}
#endif

int pool_run(void *workers, size_t size, int count, pool_task task, int result) {
  if (result == 0) {
    struct runner *runners = calloc((size_t)count, sizeof *runners);
#ifdef _WIN32
    HANDLE *threads = calloc((size_t)count, sizeof *threads);
#else
    pthread_t *threads = calloc((size_t)count, sizeof *threads);
#endif
    if (!runners || !threads) out_of_memory();
    int started = 1;
    for (; started < count; started++) {
      runners[started] = (struct runner){(char *)workers + (size_t)started * size, task};
#ifdef _WIN32
      threads[started] = CreateThread(NULL, 0, start, &runners[started], 0, NULL);
      if (!threads[started]) break;
#else
      if (pthread_create(&threads[started], NULL, start, &runners[started]) != 0) break;
#endif
    }
    work(workers, task);
    for (int i = 1; i < started; i++) {
#ifdef _WIN32
      WaitForSingleObject(threads[i], INFINITE);
      CloseHandle(threads[i]);
#else
      pthread_join(threads[i], NULL);
#endif
    }
    free(threads);
    free(runners);
    result = stop_result;
  }
  while (queued > 0) free(queue[--queued]);
  for (int i = 0; i < count; i++) {
    const struct totals *own = (const struct totals *)((char *)workers + (size_t)i * size);
    shared_totals->allocated += own->allocated;
    shared_totals->private_size += own->private_size;
    shared_totals->files += own->files;
    shared_totals->skipped += own->skipped;
    shared_totals->exact &= own->exact;
  }
  return result;
}

int count_file(struct totals *own, uint64_t dev, uint64_t ino, uint64_t ino_high, uint32_t links, uint64_t allocated,
               uint64_t private_size, int shared, const char *deferred) {
  if (links <= 1) {
    add_file(own, dev, ino, ino_high, links, allocated, private_size, shared);
    return 1;
  }
  lock(&table_lock);
  struct inode *linked = add_file(shared_totals, dev, ino, ino_high, links, allocated, private_size, shared);
  if (linked && deferred) {
    size_t length = strlen(deferred) + 1;
    linked->deferred = malloc(length);
    if (!linked->deferred) out_of_memory();
    memcpy(linked->deferred, deferred, length);
  }
  unlock(&table_lock);
  return linked != NULL;
}

void pool_seen(uint64_t files) { add_seen(files); }

void output_begin(void) { lock(&output_lock); }

void output_end(void) { unlock(&output_lock); }

int progress_begin(void) {
  if (!try_lock(&output_lock)) return 0;
  if (progress_due()) return 1;
  unlock(&output_lock);
  return 0;
}

void progress_end(const char *directory) {
  struct totals view = {0};
  view.files = load_seen();
  progress_show(&view, directory);
  unlock(&output_lock);
}

#ifndef _WIN32
static pthread_mutex_t long_path_lock;
static pthread_once_t long_path_once = PTHREAD_ONCE_INIT;

static void long_path_init(void) {
  pthread_mutexattr_t attributes;
  pthread_mutexattr_init(&attributes);
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&long_path_lock, &attributes);
  pthread_mutexattr_destroy(&attributes);
}

void long_path_begin(void) {
  pthread_once(&long_path_once, long_path_init);
  pthread_mutex_lock(&long_path_lock);
}

void long_path_end(void) { pthread_mutex_unlock(&long_path_lock); }
#endif
