#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdatomic.h>

#include "pmlog.h"
#include "pmutils.h"
#include "x86intrin.h" // for __rdtscp

static int max_num_events = 0;
static int sample_period = 0;
static FILE *live_dump = 0;
static atomic_int num_events = 0;
static pmlog_evt_t *buf;

void pmlog_init(int _max_num_events, int _sample_period, FILE *_live_dump) {
  ASSERT(_sample_period > 0);
  ASSERT(_max_num_events > 0);
  max_num_events = _max_num_events;
  sample_period = _sample_period;
  live_dump = _live_dump; // usually stdout
  num_events = 0;
  buf = malloc(max_num_events * sizeof(pmlog_evt_t));
  ASSERT(buf);
}

void pmlog_cleanup() {
  max_num_events = 0;
  sample_period = 0;
  num_events = 0;
  live_dump = NULL;
  free(buf);
}

void pmlog_record(txn_id_t txn_id, pmlog_kind_t kind, uint64_t aux_data) {
  if (txn_id % sample_period != 0) return;

  unsigned int _; // unused temp variable for rdtscp
  int i = atomic_fetch_add_explicit(&num_events, 1, memory_order_relaxed);
  ASSERT(i < max_num_events);
  buf[i] = (pmlog_evt_t){ __rdtscp(&_), txn_id, kind, aux_data };

  // TODO: print to live_dump if not null
}

static uint64_t base_tsc;
static double cpu_freq;

void pmlog_start_timer(double _cpu_freq) {
  base_tsc = __rdtsc();
  cpu_freq = cpu_freq;
}

void pmlog_write(FILE *f) {
  // TODO: write log to a file
  // preferably use multi-thread if that helps/is possible?
}

void pmlog_read(FILE *f) {
  // TODO: read log from a file to buf
  // preferably use multi-thread parsing if that helps?
}

void pmlog_dump_text(FILE *f) {
  // TODO: print human readable log to file (called in case of no live-dump)
  // double time = (double)(e->tsc - base_tsc) / cpu_freq;
  // [+0.0000001] txn_id=42 submitted
  // [+0.0000002] txn_id=42 received
  // [+0.0000003] txn_id=42 scheduled
  // [+0.0000004] txn_id=42 executing on puppet_id=0
  // [+0.0000005] txn_id=42 done on puppet_id=0
}
