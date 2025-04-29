// pmhw_sim_bloom.c - Puppetmaster simulation with Bloom filter-based conflict checking

#include "pmhw.h"
#include "bloom.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>

// --- Configuration ---

#define BLOOM_FALLBACK_EXACT_CHECK 0
#define BLOOM_REFRESH_THRESHOLD 64

#define MAX_PENDING 128
#define MAX_ACTIVE  128
#define MAX_SCHEDULED 128
#define MAX_PUPPETS 64

// --- Structures ---

typedef struct {
  pmhw_txn_t txn;
} txn_entry_t;

typedef struct {
  int transactionId;
  int puppetId;
} done_entry_t, scheduled_entry_t;

// --- Queues ---

static txn_entry_t pending_queue[MAX_PENDING];
static int pending_head = 0, pending_tail = 0;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

static done_entry_t done_queue[MAX_ACTIVE];
static int done_head = 0, done_tail = 0;
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;

static scheduled_entry_t scheduled_queue[MAX_SCHEDULED];
static int scheduled_head = 0, scheduled_tail = 0;
static pthread_mutex_t scheduled_mutex = PTHREAD_MUTEX_INITIALIZER;

static pmhw_txn_t active_txns[MAX_ACTIVE];
static int num_active = 0;

static bool puppet_free[MAX_PUPPETS];
static pthread_mutex_t puppet_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t scheduler_thread;
static volatile int scheduler_running = 0;

static bloom_t active_bloom;
static int scheduled_txn_since_refresh = 0;

// Dummy config
static pmhw_config_t dummy_config = {
  .logNumberRenamerThreads = 0,
  .logNumberShards = 0,
  .logSizeShard = 0,
  .logNumberHashes = 0,
  .logNumberComparators = 0,
  .logNumberSchedulingRounds = 0,
  .logNumberPuppets = 3,
  .numberAddressOffsetBits = 0,
  .logSizeRenamerBuffer = 0,
  .useSimulatedTxnDriver = true,
  .useSimulatedPuppets = false,
  .simulatedPuppetsClockPeriod = 1
};

// --- Helpers ---

static int queue_full(int head, int tail, int max) {
  return ((tail + 1) % max) == head;
}

static int queue_empty(int head, int tail) {
  return head == tail;
}

static int queue_length(int head, int tail, int max) {
  return head <= tail ? tail - head : max - (head - tail);
}

#if BLOOM_FALLBACK_EXACT_CHECK
static bool has_conflict_with_active(const pmhw_txn_t *pending) {
  for (int i = 0; i < num_active; ++i) {
    const pmhw_txn_t *active = &active_txns[i];
    for (int j = 0; j < pending->numWriteObjs; ++j) {
      uint64_t obj = pending->writeObjIds[j];
      for (int k = 0; k < active->numReadObjs; ++k)
        if (obj == active->readObjIds[k]) return true;
      for (int k = 0; k < active->numWriteObjs; ++k)
        if (obj == active->writeObjIds[k]) return true;
    }
    for (int j = 0; j < pending->numReadObjs; ++j) {
      uint64_t obj = pending->readObjIds[j];
      for (int k = 0; k < active->numWriteObjs; ++k)
        if (obj == active->writeObjIds[k]) return true;
    }
  }
  return false;
}
#endif

static bool bloom_conflict_check(const pmhw_txn_t *pending) {
  for (int i = 0; i < pending->numReadObjs; ++i)
    if (bloom_query(&active_bloom, pending->readObjIds[i]))
      return true;
  for (int i = 0; i < pending->numWriteObjs; ++i)
    if (bloom_query(&active_bloom, pending->writeObjIds[i]))
      return true;
  return false;
}

static int find_free_puppet() {
  static int prev = 0;
  pthread_mutex_lock(&puppet_mutex);
  int n = 1 << dummy_config.logNumberPuppets;
  for (int j = 0; j < n; ++j) {
    int i = (prev+j) % n;
    if (puppet_free[i]) {
      puppet_free[i] = false;
      pthread_mutex_unlock(&puppet_mutex);
      return i;
    }
  }
  pthread_mutex_unlock(&puppet_mutex);
  return -1;
}

static void mark_puppet_free(int puppet_id) {
  pthread_mutex_lock(&puppet_mutex);
  puppet_free[puppet_id] = true;
  pthread_mutex_unlock(&puppet_mutex);
}

static void rebuild_bloom() {
  bloom_init(&active_bloom);
  for (int i = 0; i < num_active; ++i) {
    const pmhw_txn_t *txn = &active_txns[i];
    for (int j = 0; j < txn->numReadObjs; ++j)
      bloom_insert(&active_bloom, txn->readObjIds[j]);
    for (int j = 0; j < txn->numWriteObjs; ++j)
      bloom_insert(&active_bloom, txn->writeObjIds[j]);
  }
}

extern void pin_thread_to_core(int core_id);

static void *scheduler_loop(void *arg) {
  pin_thread_to_core(2);
  (void)arg;

  while (scheduler_running) {
    if (!queue_empty(done_head, done_tail)) {
      pthread_mutex_lock(&done_mutex);
      while (!queue_empty(done_head, done_tail)) {
        done_entry_t e = done_queue[done_head];
        done_head = (done_head + 1) % MAX_ACTIVE;
        for (int i = 0; i < num_active; ++i) {
          if (active_txns[i].transactionId == e.transactionId) {
            active_txns[i] = active_txns[num_active - 1];
            num_active--;
            break;
          }
        }
        mark_puppet_free(e.puppetId);
      }
      pthread_mutex_unlock(&done_mutex);
    }

    if (!queue_empty(pending_head, pending_tail)) {
      pthread_mutex_lock(&pending_mutex);
      int n = queue_length(pending_head, pending_tail, MAX_PENDING);
      bool scheduled_something = false;
      for (int i = 0; i < n; ++i) {
        txn_entry_t *entry = &pending_queue[pending_head];
        if (entry->txn.transactionId != -1) {
          bool conflict = bloom_conflict_check(&entry->txn);
#if BLOOM_FALLBACK_EXACT_CHECK
          if (conflict && !has_conflict_with_active(&entry->txn)) {
            conflict = false;
          }
#endif
          if (!conflict) {
            int assigned_puppet = find_free_puppet();
            if (assigned_puppet >= 0) {
              if (num_active < MAX_ACTIVE) {
                active_txns[num_active++] = entry->txn;
              }
              pthread_mutex_lock(&scheduled_mutex);
              scheduled_queue[scheduled_tail] = (scheduled_entry_t){
                .transactionId = entry->txn.transactionId,
                .puppetId = assigned_puppet
              };
              scheduled_tail = (scheduled_tail + 1) % MAX_SCHEDULED;
              pthread_mutex_unlock(&scheduled_mutex);
              for (int j = 0; j < entry->txn.numReadObjs; ++j)
                bloom_insert(&active_bloom, entry->txn.readObjIds[j]);
              for (int j = 0; j < entry->txn.numWriteObjs; ++j)
                bloom_insert(&active_bloom, entry->txn.writeObjIds[j]);
              scheduled_txn_since_refresh++;
              scheduled_something = true;
              if (scheduled_txn_since_refresh >= BLOOM_REFRESH_THRESHOLD) {
                rebuild_bloom();
                scheduled_txn_since_refresh = 0;
              }
              entry->txn.transactionId = -1;
            } else {
              break;
            }
          }
        }
      }
      if (!scheduled_something && !queue_empty(pending_head, pending_tail)) {
        rebuild_bloom();
        scheduled_txn_since_refresh = 0;
      }
      while (!queue_empty(pending_head, pending_tail) && pending_queue[pending_head].txn.transactionId == -1) {
        pending_head = (pending_head + 1) % MAX_PENDING;
      }
      pthread_mutex_unlock(&pending_mutex);
    }
  }
  return NULL;
}

// --- Interface Implementations ---

pmhw_retval_t pmhw_reset() {
  pending_head = pending_tail = 0;
  done_head = done_tail = 0;
  scheduled_head = scheduled_tail = 0;
  num_active = 0;
  scheduler_running = 1;

  bloom_init(&active_bloom);
  scheduled_txn_since_refresh = 0;

  pthread_mutex_lock(&puppet_mutex);
  int n = 1 << dummy_config.logNumberPuppets;
  for (int i = 0; i < n; ++i) puppet_free[i] = true;
  pthread_mutex_unlock(&puppet_mutex);

  if (pthread_create(&scheduler_thread, NULL, scheduler_loop, NULL) != 0)
    return PMHW_NO_HW_CONN;

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

  while (1) {
    pthread_mutex_lock(&pending_mutex);
    if (!queue_full(pending_head, pending_tail, MAX_PENDING)) {
      pending_queue[pending_tail].txn = *txn;
      pending_tail = (pending_tail + 1) % MAX_PENDING;
      pthread_mutex_unlock(&pending_mutex);
      return PMHW_OK;
    }
    pthread_mutex_unlock(&pending_mutex);
  }
}

pmhw_retval_t pmhw_trigger_simulated_driver() {
  return PMHW_OK;
}

pmhw_retval_t pmhw_force_trigger_scheduling() {
  return PMHW_OK;
}

pmhw_retval_t pmhw_poll_scheduled(int *transactionId, int *puppetId) {
  if (!transactionId || !puppetId) return PMHW_INVALID_VALS;
  if (queue_empty(scheduled_head, scheduled_tail)) return PMHW_TIMEOUT;

  pthread_mutex_lock(&scheduled_mutex);
  if (!queue_empty(scheduled_head, scheduled_tail)) {
    scheduled_entry_t e = scheduled_queue[scheduled_head];
    scheduled_head = (scheduled_head + 1) % MAX_SCHEDULED;
    *transactionId = e.transactionId;
    *puppetId = e.puppetId;
    pthread_mutex_unlock(&scheduled_mutex);
    return PMHW_OK;
  }
  pthread_mutex_unlock(&scheduled_mutex);
  return PMHW_TIMEOUT;
}

pmhw_retval_t pmhw_report_done(int transactionId, int puppetId) {
  pthread_mutex_lock(&done_mutex);
  if (!queue_full(done_head, done_tail, MAX_ACTIVE)) {
    done_queue[done_tail] = (done_entry_t){.transactionId = transactionId, .puppetId = puppetId};
    done_tail = (done_tail + 1) % MAX_ACTIVE;
    pthread_mutex_unlock(&done_mutex);
    return PMHW_OK;
  }
  pthread_mutex_unlock(&done_mutex);
  return PMHW_ILLEGAL_OP;
}
