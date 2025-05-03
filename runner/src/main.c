#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include "pmlog.h"
#include "pmutils.h"
#include "workload.h"

#define MAIN_CORE 0
#define CLIENT_CORE 1
#define POLLER_CORE 2
// #define SCHEDULER_CORE 3
#define PUPPET_CORE_START 4

/* Configuration constants */

static int test_timeout_sec;
static int work_sim_us;
static int num_clients;
static int num_puppets;

static int sample_period;
static char log_filename[1000] = "log.bin";

static char dump_filename[1000];

static bool status_updates;
static bool live_dump;

/* Precomputed constants */

static double cpu_freq;
static uint64_t work_sim_cycles; // precomputed so executors know how long to simulate for

/*
Worker thread state
*/
typedef struct {
  pthread_t thread;
  int id;
  atomic_bool has_work;
  txn_id_t txn_id;
  uint64_t num_completed;

  /* optional: explicit padding so sizeof(puppet_t) == 64  */
  char _pad[64 - (sizeof(pthread_t) +
                  sizeof(int) +
                  sizeof(atomic_bool) +
                  sizeof(txn_id_t) +
                  sizeof(uint64_t)) % 64];
} puppet_t;

/*
Transaction buffer
*/
static workload_t *workload;

/*
Global state
*/
volatile int keep_polling __attribute__((aligned(64))) = 1;
static puppet_t puppets[MAX_PUPPETS];

/*
Worker thread
It waits until it sees work assigned to it then simulates working for some microseconds
*/
static void *puppet_thread(void *arg) {
  puppet_t *puppet = (puppet_t *)arg;
  int puppet_id = puppet->id;

  pin_thread_to_core(PUPPET_CORE_START + puppet_id);

  while (1) {
    // Busy-wait loop for work assignment
    // If no work is available, just spin here
    if (!atomic_load_explicit(&puppet->has_work, memory_order_acquire)) {
      continue;
    }

    // Take out the work
    int txn_id = puppet->txn_id;
    atomic_store_explicit(&puppet->has_work, false, memory_order_release);
    if (txn_id == -1) {
      break;  // Exit condition if assigned -1 (shutdown signal)
    }

    // Simulate transaction processing work by busy looping
    uint64_t start, end;
    unsigned int _;
    start = __rdtscp(&_);
    do {
      end = __rdtscp(&_);
    } while (end - start < work_sim_cycles);

    pmlog_record(txn_id, PMLOG_DONE, puppet_id);
    pmhw_report_done(txn_id, puppet_id);

    puppet->num_completed++;
  }

  return NULL;
}

/*
Thread to poll for scheduling decisions from hardware
Whenever it sees a result, it assigns it to the correct worker.
*/
static void *poller_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(POLLER_CORE);

  while (keep_polling) {
    txn_id_t txn_id = 0;
    bool found = pmhw_poll_scheduled(&txn_id);
    if (!found) continue;

    // TODO: find free puppet
    int puppet_id;
    
    puppet_t *puppet = &puppets[puppet_id];

    pmlog_record(PMLOG_WORK_RECV, txn_id, puppet_id);
    puppet->txn_id = txn_id;
    atomic_store_explicit(&puppet->has_work, true, memory_order_release);
  }
  return NULL;
}

/*
Client thread (submits transactions)
*/
static void *client_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(1);
  pmlog_start_timer(cpu_freq);

  for (int i = 0; i < workload->num_txns; ++i) {
    pmlog_record(PMLOG_SUBMIT, workload->txns[i].id, -1ULL);
    while (true) {
      bool done = pmhw_schedule(0, &workload->txns[i]);
      if (!done) {
        // retry
        continue;
      }
      break;
    }
  }

  return NULL;
}


/*
Main
*/
int main(int argc, char *argv[]) {
  pin_thread_to_core(MAIN_CORE);
  cpu_freq = measure_cpu_freq();

  // TODO: parse argument flags (use argparse?)
  // draft
  // positional: <transactions.csv>
  // --num-clients 1  (must be >0, <= NUM_CLIENTS)
  // --num-pupppets 8  (must be >0, <= NUM_PUPPETS)
  // --work-sim-us 10 (must be >=0)
  // --log log.bin (default: log.bin; binary log file location)
  //   --sample-period 4096  (0 = no log, >0 = log every x txns)
  // --dump log.txt (default: "" (no dump); human readable dump)
  // --live-dump (default: false; if enabled, print dump in real time to stdout)
  // --live-status (default: true, print mid-run status updates to stderr)
  if (argc != 3) { // TODO: not 3
    fprintf(stderr, "Usage: %s <transactions.csv> <work_sim_us>\n", argv[0]); // TODO: overhaul
    exit(1);
  }

  workload = parse_workload(argv[1]);

  pmlog_init(workload->num_txns * 5, sample_period, live_dump ? stdout : NULL);
  pmhw_init(num_clients, num_puppets); // Reminder: this creates a scheduler thread

  /*
  Start worker threads
  */

  for (int i = 0; i < num_puppets; ++i) {
    puppets[i].id = i;
    puppets[i].txn_id = -1;
    atomic_store_explicit(&puppets[i].has_work, false, memory_order_release);
    pthread_create(&puppets[i].thread, NULL, puppet_thread, &puppets[i]);
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
  for (int second = 0; second < (int) test_timeout_sec; second++) {
    int sum = 0;
    for (int i = 0; i < num_puppets; ++i) {
      sum += puppets[i].num_completed;
    }
    if (status_updates) {
      INFO("%d/%d transactions completed", sum, workload->num_txns);
    }

    if (sum == workload->num_txns) {
      done = true;
      break;
    }
    sleep(1);
  }

  // Just crash if timed out
  if (!done) {
    FATAL("Timeout after %d seconds", (int) test_timeout_sec);
  }

  /*
  Graceful cleanup
  */
  pmhw_shutdown();
  keep_polling = 0;
  pthread_join(client, NULL);
  pthread_join(poller, NULL);
  for (int i = 0; i < num_puppets; ++i) {
    // send shutdown signals (txn_id==-1)
    puppets[i].txn_id = -1ULL;
    atomic_store_explicit(&puppets[i].has_work, true, memory_order_release);
    pthread_join(puppets[i].thread, NULL);
  }

  /*
  Print human-readable timestamp reports
  */
  FILE *log_file = fopen(log_filename, "w");
  pmlog_write(log_file);
  fclose(log_file);

  FILE *dump_file = fopen(dump_filename, "w");
  pmlog_dump_text(dump_file);
  fclose(dump_file);

  /*
  Don't leak memory
  */
  free(workload);

  return 0;
}

