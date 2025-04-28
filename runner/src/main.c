#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <x86intrin.h> // for __rdtsc()
#include <sched.h>

#include "pmhw.h"

/*
Configuration
*/
#define WORK_SIMULATION_US 10    // Simulate 10 microseconds per transaction
#define TEST_TIMEOUT_SEC 20      // Timeout in seconds

/*
Fatal error reporting
*/
void fatal_error_impl(const char *filename, int line, const char *fmt, ...) {
  int use_color = isatty(fileno(stderr));
  const char *red = use_color ? "\033[1;31m" : "";
  const char *faint = use_color ? "\033[2m" : "";
  const char *reset = use_color ? "\033[0m" : "";

  fprintf(stderr, "%s[ERROR]%s %s%s:%d%s: ", red, reset, faint, filename, line, reset);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  exit(2);
}

#define FATAL(...) fatal_error_impl(__FILE__, __LINE__, __VA_ARGS__)

/*
Check result helper
*/
#ifdef SAFETY_CHECKS
#define CHECK_EXPECTED(retcode, expected) \
do { \
  pmhw_retval_t _r = (retcode); \
  pmhw_retval_t _e = (expected); \
  if (_r != _e) { \
    FATAL("pmhw_retval_t returned %d but expected %d", (int)_r, (int)_e); \
  } \
} while (0)
#else
#define CHECK_EXPECTED(retcode, expected) \
do { \
  (void) (retcode); \
  (void) (expected); \
} while (0)
#endif
#define CHECK_OK(retcode) CHECK_EXPECTED(retcode, PMHW_OK)

/*
Timing utilities
*/
static double cpu_ghz = 0.0;
static uint64_t base_rdtsc = 0;

void initialize_timer() {
  struct timespec ts_start, ts_end;
  uint64_t start = __rdtsc();
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  usleep(100000); // Sleep 100ms
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  uint64_t end = __rdtsc();
  double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
  cpu_ghz = (end - start) / (elapsed * 1e9);
  base_rdtsc = __rdtsc();
}

double now() {
  return (double)(__rdtsc() - base_rdtsc) / (cpu_ghz * 1e9);
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
  double timestamp;
  int transactionId;
  int puppetId; // Only valid for scheduled/done
} event_t;

event_t *event_list;
int event_index = 0;
int max_total_events = 0;

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
  if (e->kind == EVENT_SUBMIT) {
    printf("[+%9.6f] submit txn id=%d\n", e->timestamp, e->transactionId);
  } else if (e->kind == EVENT_SCHEDULED) {
    printf("[+%9.6f] scheduled txn id=%d assigned to puppet %d\n", e->timestamp, e->transactionId, e->puppetId);
  } else if (e->kind == EVENT_DONE) {
    printf("[+%9.6f] done puppet %d finished txn id=%d\n", e->timestamp, e->puppetId, e->transactionId);
  }
}

/*
Worker thread state
*/
typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  int transactionId;
  bool hasWork;
  int puppetId;
  int completed_txns;
} worker_t;

/*
Transaction buffer
*/
pmhw_txn_t *txn_list;
int num_txns = 0;

/*
Global state
*/
volatile int keep_polling = 1;
worker_t *puppets;
int num_puppets;

/*
CPU pinning helper
*/
void pin_thread_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
    FATAL("Failed to pin thread to core %d", core_id);
  }
}

/*
Record event
*/
void record_event(event_kind_t kind, double timestamp, int txn_id, int puppet_id) {
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
void *puppet_worker_thread(void *arg) {
  worker_t *worker = (worker_t *)arg;

  pin_thread_to_core(4 + worker->puppetId);

  while (1) {
    // Busy-wait loop for work assignment
    // If no work is available, just spin here
    pthread_mutex_lock(&worker->mutex);
    if (!worker->hasWork) {
      pthread_mutex_unlock(&worker->mutex);
      continue;
    }

    int txnId = worker->transactionId;
    worker->hasWork = false;
    if (txnId == -1) {
      pthread_mutex_unlock(&worker->mutex);
      break;  // Exit condition if assigned -1 (shutdown signal)
    }

    // Simulate transaction processing work by busy looping
    double start, end;
    start = now();
    do {
      end = now();
    } while ((end - start) * 1e6 < WORK_SIMULATION_US);

    record_event(EVENT_DONE, now(), txnId, worker->puppetId);
    CHECK_OK(pmhw_report_done(txnId, worker->puppetId));

    worker->completed_txns++;

    pthread_mutex_unlock(&worker->mutex);
  }

  return NULL;
}

/*
Thread to poll for scheduling decisions from hardware
Whenever it sees a result, it assigns it to the correct worker.
*/
void *poller_thread(void *arg) {
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

    pthread_mutex_lock(&worker->mutex);
    if (worker->hasWork) {
      FATAL("Puppet %d already has work assigned", puppetId);
    }
    record_event(EVENT_SCHEDULED, now(), txnId, puppetId);
    worker->transactionId = txnId;
    worker->hasWork = true;
    pthread_mutex_unlock(&worker->mutex);
  }
  return NULL;
}

/*
Client thread (submits transactions)
*/
void *client_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(1);
  initialize_timer();

  for (int i = 0; i < num_txns; ++i) {
    record_event(EVENT_SUBMIT, now(), txn_list[i].transactionId, -1);
    CHECK_OK(pmhw_schedule(&txn_list[i]));
  }

  return NULL;
}

/*
Parse CSV transaction
*/
int count_lines(FILE *f) {
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
    fprintf(stderr, "Usage: %s <transactions.csv> <period>\n", argv[0]);
    exit(1);
  }
  pin_thread_to_core(0);

  /*
  Hardware setup
  */

  pmhw_reset(); // Reminder: this does create a new thread (likely doing nothing)
  pmhw_config_t config;
  CHECK_OK(pmhw_get_config(&config));
  config.useSimulatedPuppets = false;
  config.useSimulatedTxnDriver = false;
  config.simulatedPuppetsClockPeriod = atoi(argv[2]);
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
    pthread_mutex_init(&puppets[i].mutex, NULL);
    puppets[i].transactionId = -1;
    puppets[i].hasWork = false;
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
    pthread_mutex_lock(&puppets[i].mutex);
    puppets[i].transactionId = -1;
    puppets[i].hasWork = true;
    pthread_mutex_unlock(&puppets[i].mutex);
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

