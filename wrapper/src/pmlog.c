#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#ifdef __cplusplus
#include <atomic>
using namespace std;
#else
#include <stdatomic.h>
#endif

#include "pmlog.h"
#include "pmutils.h"
#include "x86intrin.h" // for __rdtscp

static int max_num_events = 0;
static int sample_period = 0;
static FILE *live_dump = NULL;
static pthread_mutex_t live_dump_mutex;
static atomic_int num_events = 0;
pmlog_evt_t *pmlog_evt_buf;
static double cpu_freq;
static uint64_t base_tsc;

void pmlog_init(int _max_num_events, int _sample_period, FILE *_live_dump) {
  ASSERT(_sample_period >= 0);
  ASSERT(_max_num_events > 0);
  max_num_events = _max_num_events;
  sample_period = _sample_period;
  live_dump = _live_dump; // usually stdout
  num_events = 0;
  pmlog_evt_buf = (pmlog_evt_t*) malloc(max_num_events * sizeof(pmlog_evt_t));
  ASSERT(pmlog_evt_buf);
  pthread_mutex_init(&live_dump_mutex, NULL);
}

void pmlog_cleanup() {
  max_num_events = 0;
  sample_period = 0;
  num_events = 0;
  live_dump = NULL;
  free(pmlog_evt_buf);
  pthread_mutex_destroy(&live_dump_mutex);
}

static const char *kind_to_str(pmlog_kind_t k) {
  switch (k) {
    case PMLOG_SUBMIT:      return "submitted";
    case PMLOG_SCHED_READY: return "scheduled";
    case PMLOG_WORK_RECV:   return "executing";
    case PMLOG_DONE:        return "done";
    case PMLOG_CLEANUP:     return "removed";
  }
  ASSERT(false);
}

static void dump_event_human(FILE *dst, const pmlog_evt_t *e) {
  pthread_mutex_lock(&live_dump_mutex);
  double us = (e->tsc - base_tsc) / cpu_freq; /* cpu_freq Hz -> s */
  fprintf(dst, "[+%.8f] txn_id=%" PRIu64 " %s", us, e->txn_id, kind_to_str(e->kind));
  if (e->kind == PMLOG_SCHED_READY || e->kind == PMLOG_WORK_RECV || e->kind == PMLOG_DONE) {
    fprintf(dst, " on puppet_id=%" PRIu64, e->aux_data);
  }
  fputc('\n', dst);
  pthread_mutex_unlock(&live_dump_mutex);
}

void pmlog_record(txn_id_t txn_id, pmlog_kind_t kind, uint64_t aux_data) {
  if (sample_period == 0) return;
  if (txn_id % sample_period != 0) return;

  unsigned int _; // unused temp variable for rdtscp
  int i = atomic_fetch_add_explicit(&num_events, 1, memory_order_relaxed);
  // ASSERTF(i < max_num_events, "got %d expected < %d", num_events, max_num_events);

  pmlog_evt_buf[i] = (pmlog_evt_t){ __rdtscp(&_), txn_id, kind, aux_data };

  if (live_dump) {
    dump_event_human(live_dump, &pmlog_evt_buf[i]);
    fflush(live_dump);
  }
}

void pmlog_start_timer(double _cpu_freq) {
  unsigned int _; // unused temp variable for rdtscp
  base_tsc = __rdtscp(&_);
  cpu_freq = _cpu_freq;
}

static int compare_events(const void *a, const void *b) {
  const pmlog_evt_t *ea = (const pmlog_evt_t *)a;
  const pmlog_evt_t *eb = (const pmlog_evt_t *)b;
  if (ea->tsc < eb->tsc) return -1;
  if (ea->tsc > eb->tsc) return 1;
  return 0;
}

void pmlog_write(FILE *f) {
  qsort(pmlog_evt_buf, num_events, sizeof(pmlog_evt_t), compare_events);
  (void) fwrite(&num_events, sizeof(int), 1, f);
  (void) fwrite(&base_tsc, sizeof(uint64_t), 1, f);
  (void) fwrite(&cpu_freq, sizeof(double), 1, f);
  (void) fwrite(pmlog_evt_buf, sizeof(pmlog_evt_t), num_events, f);
}

int pmlog_read(FILE *f, double *_cpu_freq, uint64_t *_base_tsc) {
  (void) fread(&num_events, sizeof(int), 1, f);
  (void) fread(&base_tsc, sizeof(uint64_t), 1, f);
  (void) fread(&cpu_freq, sizeof(double), 1, f);
  if (_cpu_freq) *_cpu_freq = cpu_freq;
  if (_base_tsc) *_base_tsc = base_tsc;
  if (num_events > max_num_events) {
    pmlog_evt_buf = (pmlog_evt_t*) realloc(pmlog_evt_buf, num_events * sizeof(pmlog_evt_t));
    max_num_events = num_events;
  }
  (void) fread(pmlog_evt_buf, sizeof(pmlog_evt_t), num_events, f);
  return num_events;
}

void pmlog_dump_text(FILE *f) {
  qsort(pmlog_evt_buf, num_events, sizeof(pmlog_evt_t), compare_events);
  for (int i = 0; i < (int)atomic_load_explicit(&num_events, memory_order_relaxed); ++i) {
    dump_event_human(f, &pmlog_evt_buf[i]);
  }
}
