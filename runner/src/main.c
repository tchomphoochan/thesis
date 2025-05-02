#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <x86intrin.h> // for __rdtsc()
#include <sched.h>
#include <sys/sysinfo.h>

#include "pmhw.h"

/*
Configuration
*/
#define TEST_TIMEOUT_SEC 10      // Timeout in seconds

static int work_sim_us;
static uint64_t work_sim_cycles;

/*
Printing
*/
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;
static void log_message(const char *filename, int line, bool error, const char *header, const char *fmt, ...) {
  int use_color = isatty(fileno(stderr));
  const char *red = use_color ? "\033[1;31m" : "";
  const char *yellow = use_color ? "\033[1;33m" : "";
  const char *color = error ? red : yellow;
  const char *faint = use_color ? "\033[2m" : "";
  const char *reset = use_color ? "\033[0m" : "";


  pthread_mutex_lock(&print_mutex);
  fprintf(stderr, "%s[%s]%s %s%s:%d%s: ", color, header, reset, faint, filename, line, reset);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  pthread_mutex_unlock(&print_mutex);
  if (error) exit(2);
}

#define FATAL(...) log_message(__FILE__, __LINE__, true,  "ERROR", __VA_ARGS__)
#define WARN(...)  log_message(__FILE__, __LINE__, false, "WARN",  __VA_ARGS__)
#define INFO(...)  log_message(__FILE__, __LINE__, false, "INFO",  __VA_ARGS__)

/*
Check result helper
*/
#define CHECK_EXPECTED(retcode, expected) \
do { \
  pmhw_retval_t _r = (retcode); \
  pmhw_retval_t _e = (expected); \
  if (_r != _e) { \
    FATAL("pmhw_retval_t returned %d but expected %d", (int)_r, (int)_e); \
  } \
} while (0)
#define CHECK_OK(retcode) CHECK_EXPECTED(retcode, PMHW_OK)

/*
Timing utilities
*/
static double cpu_freq = 0.0;
static uint64_t base_rdtsc = 0;

static void initialize_timer() {
  struct timespec ts_start, ts_end;
  uint64_t start = __rdtsc();
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  usleep(100000); // Sleep 100ms
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  uint64_t end = __rdtsc();
  double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
  cpu_freq = (end - start) / elapsed;
  work_sim_cycles = (uint64_t)((work_sim_us * 1e-6) * cpu_freq);
  base_rdtsc = __rdtsc();
}

static uint64_t now() {
  return __rdtsc();
}

/*
Event tracking
*/
typedef enum {
  EVENT_SUBMIT,
  EVENT_SCHEDULED,
  EVENT_DONE
} event_kind_t;

typedef struct {
  event_kind_t kind;
  uint64_t timestamp;
  int transactionId;
  int puppetId; // Only valid for scheduled/done
} event_t;

static event_t *event_list;
static int event_index = 0;
static int max_total_events = 0;

/*
For sorting events
*/
static int compare_events(const void *a, const void *b) {
  const event_t *ea = (const event_t *)a;
  const event_t *eb = (const event_t *)b;
  if (ea->timestamp < eb->timestamp) return -1;
  if (ea->timestamp > eb->timestamp) return 1;
  return 0;
}

/*
Print event
*/
static void print_event(const event_t *e) {
  double time = (double)(e->timestamp - base_rdtsc) / cpu_freq;
  if (e->kind == EVENT_SUBMIT) {
    printf("[+%9.7f] submit txn id=%d\n", time, e->transactionId);
  } else if (e->kind == EVENT_SCHEDULED) {
    printf("[+%9.7f] scheduled txn id=%d assigned to puppet %d\n", time, e->transactionId, e->puppetId);
  } else if (e->kind == EVENT_DONE) {
    printf("[+%9.7f] done puppet %d finished txn id=%d\n", time, e->puppetId, e->transactionId);
  }
}

/*
Worker thread state
*/
typedef struct {
  pthread_t thread;
  int transactionId;
  atomic_bool hasWork;
  int puppetId;
  int completed_txns;
} worker_t;

/*
Transaction buffer
*/
static pmhw_txn_t *txn_list;
static int num_txns = 0;

/*
Global state
*/
volatile int keep_polling __attribute__((aligned(64))) = 1;
static worker_t *puppets;
static int num_puppets;

/*
CPU pinning helper
*/
void pin_thread_to_core(int core_id) {
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

/*
Record event
*/
static void record_event(event_kind_t kind, uint64_t timestamp, int txn_id, int puppet_id) {
  int idx = __sync_fetch_and_add(&event_index, 1);
  event_list[idx] = (event_t){
    .kind = kind,
    .timestamp = timestamp,
    .transactionId = txn_id,
    .puppetId = puppet_id
  };
#ifdef LIVE_PRINT
  print_event(&event_list[idx]);
#endif
}

/*
Worker thread
It waits until it sees work assigned to it then simulates working for some microseconds
*/
static void *puppet_worker_thread(void *arg) {
  worker_t *worker = (worker_t *)arg;

  pin_thread_to_core(4 + worker->puppetId);

  while (1) {
    // Busy-wait loop for work assignment
    // If no work is available, just spin here
    if (!atomic_load_explicit(&worker->hasWork, memory_order_acquire)) {
      continue;
    }

    int txnId = worker->transactionId;
    atomic_store_explicit(&worker->hasWork, false, memory_order_release);
    if (txnId == -1) {
      break;  // Exit condition if assigned -1 (shutdown signal)
    }

    // Simulate transaction processing work by busy looping
    uint64_t start, end;
    start = now();
    do {
      end = now();
    } while (end - start < work_sim_cycles);

    record_event(EVENT_DONE, now(), txnId, worker->puppetId);
    CHECK_OK(pmhw_report_done(txnId, worker->puppetId));

    worker->completed_txns++;
  }

  return NULL;
}

/*
Thread to poll for scheduling decisions from hardware
Whenever it sees a result, it assigns it to the correct worker.
*/
static void *poller_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(3);

  while (keep_polling) {
    int txnId = 0, puppetId = 0;
    pmhw_retval_t status = pmhw_poll_scheduled(&txnId, &puppetId);

    if (status == PMHW_TIMEOUT) {
      continue;
    } else if (status != PMHW_OK) {
      FATAL("pmhw_poll_scheduled failed with status %d", status);
    }

    worker_t *worker = &puppets[puppetId];

    if (atomic_load_explicit(&worker->hasWork, memory_order_acquire)) {
      FATAL("Puppet %d already has work assigned", puppetId);
    }
    record_event(EVENT_SCHEDULED, now(), txnId, puppetId);
    worker->transactionId = txnId;
    atomic_store_explicit(&worker->hasWork, true, memory_order_release);
  }
  return NULL;
}

/*
Client thread (submits transactions)
*/
static void *client_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(1);
  initialize_timer();

  for (int i = 0; i < num_txns; ++i) {
    record_event(EVENT_SUBMIT, now(), txn_list[i].transactionId, -1);
    while (true) {
      pmhw_retval_t ret = pmhw_schedule(&txn_list[i]);
      if (ret == PMHW_TIMEOUT) { continue; }
      if (ret != PMHW_OK) { FATAL("client_thread failed"); }
      break;
    }
  }

  return NULL;
}

/*
Parse CSV transaction
*/
static int count_lines(FILE *f) {
  int count = 0;
  char buf[1000];
  while (fgets(buf, sizeof(buf), f)) {
    if (strlen(buf) > 1) count++;
  }
  return count;
}

/*
Parse CSV transaction of format auxData,oid0,rw0,oid1,rw1,...
wherer rw flags are 0=read, 1=write
*/
static void parse_txn(pmhw_txn_t *txn, int id, const char *buf) {
  txn->transactionId = id;
  txn->numReadObjs = 0;
  txn->numWriteObjs = 0;

  const char *p = buf;
  int objid, writeflag;

  uint64_t aux_data;
  if (sscanf(p, "%lu", &aux_data) != 1) {
    FATAL("Failed to parse aux_data");
  }
  txn->auxData = aux_data;

  while (*p && *p != ',') p++;
  if (*p == ',') p++;

  while (*p) {
    if (sscanf(p, "%d", &objid) != 1) {
      FATAL("Failed to parse objid");
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    if (sscanf(p, "%d", &writeflag) != 1) {
      FATAL("Failed to parse writeflag");
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    if (writeflag) {
      txn->writeObjIds[txn->numWriteObjs++] = objid;
    } else {
      txn->readObjIds[txn->numReadObjs++] = objid;
    }
  }
}

/*
Main
*/
int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <transactions.csv> <work_sim_us>\n", argv[0]);
    exit(1);
  }
  pthread_mutex_init(&print_mutex, NULL);
  pin_thread_to_core(0);

  /*
  Hardware setup
  */

  pmhw_reset(); // Reminder: this does create a new thread (likely doing nothing)
  pmhw_config_t config;
  CHECK_OK(pmhw_get_config(&config));
  config.useSimulatedPuppets = false;
  config.useSimulatedTxnDriver = false;
  // config.simulatedPuppetsClockPeriod = atoi(argv[2]);
  work_sim_us = atoi(argv[2]);
  CHECK_EXPECTED(pmhw_set_config(&config), PMHW_PARTIAL);

  /*
  Parse transactions
  */

  num_puppets = 1 << config.logNumberPuppets;

  FILE *f = fopen(argv[1], "r");
  if (!f) FATAL("Failed to open transaction file");

  num_txns = count_lines(f);
  rewind(f);
  txn_list = (pmhw_txn_t*) malloc(sizeof(pmhw_txn_t) * num_txns);
  if (!txn_list) FATAL("Failed to malloc txn_list");

  max_total_events = 3 * num_txns;
  event_list = (event_t*) malloc(sizeof(event_t) * max_total_events);
  if (!event_list) FATAL("Failed to malloc event_list");

  char buf[1000];
  int id = 0;
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf) > 0) {
      parse_txn(&txn_list[id], id, buf);
      id++;
    }
  }
  fclose(f);

  /*
  Start worker threads
  */

  puppets = (worker_t*) calloc(num_puppets, sizeof(worker_t));
  if (!puppets) FATAL("Failed to calloc puppets");

  for (int i = 0; i < num_puppets; ++i) {
    puppets[i].puppetId = i;
    puppets[i].transactionId = -1;
    atomic_store_explicit(&puppets[i].hasWork, false, memory_order_release);
  }

  for (int i = 0; i < num_puppets; ++i) {
    pthread_create(&puppets[i].thread, NULL, puppet_worker_thread, &puppets[i]);
  }

  /*
  Start poller and client
  */

  pthread_t poller;
  pthread_create(&poller, NULL, poller_thread, NULL);

  pthread_t client;
  pthread_create(&client, NULL, client_thread, NULL); // client_thread starts the timer

  /*
  Wait until we're sure everything is done
  */

  // Every second, check whether everything has finished so we can break out early
  bool done = false;
  for (int second = 0; second < (int) TEST_TIMEOUT_SEC; second++) {
    int sum = 0;
    for (int i = 0; i < num_puppets; ++i) {
      sum += puppets[i].completed_txns;
    }
    INFO("%d/%d transactions completed", sum, num_txns);

    if (sum == num_txns) {
      done = true;
      break;
    }
    sleep(1);
  }

  // Just crash
  if (!done) {
    FATAL("Timeout after %d seconds", (int) TEST_TIMEOUT_SEC);
  }

  /*
  Graceful cleanup
  */

  keep_polling = 0;
  pthread_join(client, NULL);
  pthread_join(poller, NULL);

  for (int i = 0; i < num_puppets; ++i) {
    puppets[i].transactionId = -1;
    atomic_store_explicit(&puppets[i].hasWork, true, memory_order_release);
  }
  for (int i = 0; i < num_puppets; ++i) {
    pthread_join(puppets[i].thread, NULL);
  }

  /*
  Print timestamp reports
  */
#ifndef LIVE_PRINT

  qsort(event_list, event_index, sizeof(event_t), compare_events);

  for (int i = 0; i < event_index; ++i) {
    print_event(&event_list[i]);
  }
#endif

  /*
  Don't leak memory
  */

  free(txn_list);
  free(event_list);
  free(puppets);

  return 0;
}

