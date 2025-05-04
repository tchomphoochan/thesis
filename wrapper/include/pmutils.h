#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <x86intrin.h>

static pthread_mutex_t _stderr_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void log_message(const char *filename, int line, const char *color, const char *header, const char *fmt, ...) {
  pthread_mutex_lock(&_stderr_mutex);
  bool use_color = (bool) isatty(fileno(stderr));
  const char *_color = use_color ? color : "";
  const char *faint = use_color ? "\033[2m" : "";
  const char *reset = use_color ? "\033[0m" : "";

  fprintf(stderr, "%s[%s]%s %s%s:%d%s: ", _color, header, reset, faint, filename, line, reset);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  pthread_mutex_unlock(&_stderr_mutex);
}

#define ERROR(...) log_message(__FILE__, __LINE__, "\033[1;31m", "ERROR", __VA_ARGS__)
#define WARN(...)  log_message(__FILE__, __LINE__, "\033[1;33m", "WARN",  __VA_ARGS__)
#define INFO(...)  log_message(__FILE__, __LINE__, "\033[1;37m", "INFO",  __VA_ARGS__)
// #define DEBUG_MSG(...)  log_message(__FILE__, __LINE__, "\033[0;37m", "DEBUG",  __VA_ARGS__)
#define DEBUG_MSG(...)  
#define FATAL(...) do { ERROR(__VA_ARGS__); exit(1); } while (0)

#define ASSERTF(condition, ...) do { if (!(condition)) { FATAL(__VA_ARGS__); } } while (0)
#define ASSERT(condition) ASSERTF(condition, "Assertion failed")
#define EXPECT_OK(condition) ASSERTF(condition, "Unexpected failure")

static inline void pin_thread_to_core(int core_id) {
  int n = get_nprocs();
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id % n, &cpuset);
  if (core_id >= n) {
    WARN("Cannot pin thread to core %d. Pinned to %d instead.", core_id, core_id % n);
  }
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
    FATAL("Failed to pin thread to core %d", core_id % n);
  }
}

static inline double measure_cpu_freq() {
  struct timespec ts_start, ts_end;
  uint64_t start = __rdtsc();
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  usleep(100000); // Sleep 100ms
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  uint64_t end = __rdtsc();
  double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
  return (end - start) / elapsed;
}

#ifdef __cplusplus
}
#endif
