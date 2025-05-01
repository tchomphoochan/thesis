// pmhw_sim.c - Pure C simulation of Puppetmaster interface

#define _GNU_SOURCE
#include "pmhw.h"
#include "spsc_queue.h"
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <sys/sysinfo.h>

#define MAX_PENDING 128
#define MAX_ACTIVE 128
#define MAX_SCHEDULED 128
#define MAX_PUPPETS 64

/*
Printing
*/
static void log_message(const char *filename, int line, bool error, const char *header, const char *fmt, ...) {
  int use_color = isatty(fileno(stderr));
  const char *red = use_color ? "\033[1;31m" : "";
  const char *yellow = use_color ? "\033[1;33m" : "";
  const char *color = error ? red : yellow;
  const char *faint = use_color ? "\033[2m" : "";
  const char *reset = use_color ? "\033[0m" : "";


  fprintf(stderr, "%s[%s]%s %s%s:%d%s: ", color, header, reset, faint, filename, line, reset);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  if (error) exit(2);
}

#define FATAL(...) log_message(__FILE__, __LINE__, true,  "ERROR", __VA_ARGS__)
#define WARN(...)  log_message(__FILE__, __LINE__, false, "WARN",  __VA_ARGS__)

#define CHECK(x) \
  do { \
    bool _x = (x); \
    if (!_x) { \
      FATAL("Condition is unexpectedly unsatisfied"); \
    } \
  } while (0)


// === Internal Structures ===

typedef struct {
  pmhw_txn_t txn;
} txn_entry_t;

typedef struct {
  int transactionId;
} done_entry_t;

typedef struct {
  int transactionId;
} scheduled_entry_t;

// === Queues ===
//
SPSC_QUEUE_IMPL(txn_entry_t, PendingQ)
SPSC_QUEUE_IMPL(done_entry_t, DoneQ)
SPSC_QUEUE_IMPL(scheduled_entry_t, ScheduledQ)

static PendingQ pending_queue;
static DoneQ done_queue;
static ScheduledQ scheduled_queue;

pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;

static pmhw_txn_t active_txns[MAX_ACTIVE];
static int num_active = 0;

static pthread_t scheduler_thread;
static volatile int scheduler_running = 0;

// Dummy configuration
static pmhw_config_t dummy_config = {
  .logNumberRenamerThreads = 0,
  .logNumberShards = 0,
  .logSizeShard = 0,
  .logNumberHashes = 0,
  .logNumberComparators = 0,
  .logNumberSchedulingRounds = 0,
  .logNumberPuppets = 3, // 8 puppets (UNUSED NOW)
  .numberAddressOffsetBits = 0,
  .logSizeRenamerBuffer = 0,
  .useSimulatedTxnDriver = true,
  .useSimulatedPuppets = false,
  .simulatedPuppetsClockPeriod = 1
};

static bool has_conflict(const pmhw_txn_t *a, const pmhw_txn_t *b) {
  for (int i = 0; i < a->numWriteObjs; ++i) {
    uint64_t obj = a->writeObjIds[i];
    for (int j = 0; j < b->numReadObjs; ++j)
      if (obj == b->readObjIds[j]) return true;
    for (int j = 0; j < b->numWriteObjs; ++j)
      if (obj == b->writeObjIds[j]) return true;
  }
  for (int i = 0; i < a->numReadObjs; ++i) {
    uint64_t obj = a->readObjIds[i];
    for (int j = 0; j < b->numWriteObjs; ++j)
      if (obj == b->writeObjIds[j]) return true;
  }
  return false;
}

static bool conflicts_with_active(const pmhw_txn_t *pending) {
  for (int i = 0; i < num_active; ++i) {
    if (has_conflict(pending, &active_txns[i])) {
      return true;
    }
  }
  return false;
}

// === Scheduler Thread ===

static void pin_thread_to_core(int core_id) {
  int n = get_nprocs();
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id % n, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
    FATAL("Failed to pin thread to core %d", core_id);
  }
  if (core_id >= n) {
    WARN("Cannot pin thread to core %d. Pinned to %d instead.", core_id, core_id % n);
  }
}

static void *scheduler_loop(void *arg) {
  pin_thread_to_core(2);
  (void)arg;

  while (scheduler_running) {
    // Drain done queue (applications tells us it has finished executing a transaction)
    done_entry_t done_entry;
    while (spsc_dequeue_DoneQ(&done_queue, &done_entry)) {
      bool found = false;
      for (int i = 0; i < num_active; ++i) {
        // Remove that transaction from list of active transactions
        // (swap with the last one and decrease counter)
        if (active_txns[i].transactionId == done_entry.transactionId) {
          active_txns[i] = active_txns[num_active - 1];
          num_active--;
          found = true;
          break;
        }
      }
      CHECK(found);
    }

    if (num_active >= MAX_ACTIVE) { continue; }

    // Check pending uqeue.
    txn_entry_t pending_entry;
    if (!spsc_peek_PendingQ(&pending_queue, &pending_entry)) { continue; }
    if (conflicts_with_active(&pending_entry.txn)) { continue; }

    // Try to enqueue to scheduled queue
    scheduled_entry_t scheduled_entry = {
      .transactionId = pending_entry.txn.transactionId
    };
    if (!spsc_enqueue_ScheduledQ(&scheduled_queue, &scheduled_entry)) { continue; }

    // Move transaction to active_txns (we know we have space)
    // and schedule it
    active_txns[num_active++] = pending_entry.txn;
    CHECK(spsc_dequeue_PendingQ(&pending_queue, &pending_entry));
  }
  return NULL;
}

// === Interface Implementations ===

pmhw_retval_t pmhw_reset() {
  spsc_queue_init_PendingQ(&pending_queue, MAX_PENDING);
  spsc_queue_init_DoneQ(&done_queue, MAX_ACTIVE);
  spsc_queue_init_ScheduledQ(&scheduled_queue, MAX_SCHEDULED);
  pthread_mutex_init(&done_mutex, NULL);

  num_active = 0;
  scheduler_running = 1;

  if (pthread_create(&scheduler_thread, NULL, scheduler_loop, NULL) != 0) {
    return PMHW_NO_HW_CONN;
  }

  return PMHW_OK;
}

pmhw_retval_t pmhw_get_config(pmhw_config_t *ret) {
  if (!ret) return PMHW_INVALID_VALS;
  *ret = dummy_config;
  return PMHW_OK;
}

pmhw_retval_t pmhw_set_config(const pmhw_config_t *cfg) {
  if (!cfg) return PMHW_INVALID_VALS;
  if (cfg->useSimulatedTxnDriver || cfg->useSimulatedPuppets)
    return PMHW_INVALID_VALS;
  dummy_config = *cfg;
  return PMHW_PARTIAL;
}

pmhw_retval_t pmhw_schedule(const pmhw_txn_t *txn) {
  if (!txn) return PMHW_INVALID_VALS;
  txn_entry_t e = { .txn = *txn };
  if (spsc_enqueue_PendingQ(&pending_queue, &e)) {
    return PMHW_OK;
  }
  return PMHW_TIMEOUT;
}

pmhw_retval_t pmhw_trigger_simulated_driver() {
  return PMHW_OK;
}

pmhw_retval_t pmhw_force_trigger_scheduling() {
  return PMHW_OK;
}

pmhw_retval_t pmhw_poll_scheduled(int *transactionId) {
  if (!transactionId) return PMHW_INVALID_VALS;
  scheduled_entry_t e;
  if (spsc_dequeue_ScheduledQ(&scheduled_queue, &e)) {
    *transactionId = e.transactionId;
    return PMHW_OK;
  }
  return PMHW_TIMEOUT;
}

pmhw_retval_t pmhw_report_done(int transactionId) {
  pthread_mutex_lock(&done_mutex);
  done_entry_t e = {.transactionId = transactionId };
  if (spsc_enqueue_DoneQ(&done_queue, &e)) {
    pthread_mutex_unlock(&done_mutex);
    return PMHW_OK;
  }
  pthread_mutex_unlock(&done_mutex);
  return PMHW_ILLEGAL_OP;
}

