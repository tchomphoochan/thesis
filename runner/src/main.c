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
#include <getopt.h>


#include "pmhw.h"
#include "pmlog.h"
#include "pmutils.h"
#include "workload.h"

// #define SCHEDULER_CORE 0
#define MAIN_CORE 1
#define CLIENT_CORE 2
#define PUPPET_CORE_START 3

/*
Configuration
*/

#define DEF_TIMEOUT_SEC     30
#define DEF_WORK_US         0
#define DEF_NUM_CLIENTS     1
#define DEF_NUM_PUPPETS     8
#define DEF_SAMPLE_SHIFT    0
#define DEF_WORKLOAD_FILE   "transactions.csv"
#define DEF_LOG_FILE        ""
#define DEF_DUMP_FILE       ""

static const char *usage =
  "Usage: main [options]\n"
  "  --input FILE         Transaction CSV file (default transactions.csv)\n"
  "  --timeout SEC        Benchmark wall‑clock duration (default 10)\n"
  "  --work-us USEC       Simulated work per txn (default 0)\n"
  "  --clients N          Number of client threads (default 1)\n"
  "  --puppets N          Number of worker (puppet) threads (default 8)\n"
  "  --sample-shift S     Log 1 event every 2^S txns (default 0)\n"
  "  --log FILE           Binary log output (if set)\n"
  "  --dump FILE          Human dump after run (if set)\n"
  "  --status             Periodic stderr status (every second)\n"
  "  --live-dump          Print events as they happen (stdout)\n"
  "  --help\n";

static int test_timeout_sec = DEF_TIMEOUT_SEC;
static int work_sim_us      = DEF_WORK_US;
static int num_clients      = DEF_NUM_CLIENTS;
static int num_puppets      = DEF_NUM_PUPPETS;

static int  sample_period           = 1 << DEF_SAMPLE_SHIFT;
static char log_filename[1000]      = DEF_LOG_FILE;
static char dump_filename[1000]     = DEF_DUMP_FILE;
static char workload_filename[1000] = DEF_WORKLOAD_FILE;

static bool status_updates = false;
static bool live_dump      = false;
static bool limit_client   = false; // limit client throughput for better latency measurements

static double   cpu_freq        = 0.0;  // set at beginning of main
static uint64_t work_sim_cycles = 0;    // ditto


/*
Worker thread state
*/
typedef struct {
  pthread_t thread;
  int id;
  uint64_t num_completed;

  /* optional: explicit padding so sizeof(puppet_t) == 64  */
  char _pad[64 - (sizeof(pthread_t) +
                  sizeof(int) +
                  sizeof(uint64_t)) % 64];
} puppet_t;

/*
Transaction buffer
*/
static workload_t *workload;

/*
Global state
*/
volatile atomic_bool keep_polling __attribute__((aligned(64))) = ATOMIC_VAR_INIT(true);
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
    // Poll for work assignment
    txn_id_t txn_id;
    if (!pmhw_poll_scheduled(puppet_id, &txn_id)) break;

    pmlog_record(txn_id, PMLOG_WORK_RECV, puppet_id);

    // Simulate transaction processing work by busy looping
    uint64_t start, end;
    start = __rdtsc();
    do {
      end = __rdtsc();
    } while (end - start < work_sim_cycles);

    pmhw_report_done(puppet_id, txn_id);

    puppet->num_completed++;
  }

  return NULL;
}

/*
Client thread (submits transactions)
*/
static void *client_thread(void *arg) {
  (void)arg;

  pin_thread_to_core(CLIENT_CORE);
  pmlog_start_timer(cpu_freq);

  uint64_t client_sim_cycles = work_sim_cycles;
  if (work_sim_cycles == 0) client_sim_cycles = cpu_freq * 1e-6 / num_puppets;

  for (int i = 0; i < workload->num_txns; ++i) {
    pmhw_schedule(0, &workload->txns[i]);

    if (limit_client && client_sim_cycles > 0) {
      uint64_t start, end;
      start = __rdtsc();
      do {
        end = __rdtsc();
      } while (end - start < client_sim_cycles);
    }
  }

  return NULL;
}

/*
Parse command line arguments
*/
static void parse_args(int argc, char **argv) {
  if (argc == 1) {
    fputs(usage, stderr);
    exit(0);
  }

  static struct option opts[] = {
    {"input",        required_argument, 0, 'f'},
    {"timeout",      required_argument, 0, 't'},
    {"work-us",      required_argument, 0, 'w'},
    {"clients",      required_argument, 0, 'c'},
    {"puppets",      required_argument, 0, 'p'},
    {"sample-shift", required_argument, 0, 's'},
    {"log",          required_argument, 0, 'l'},
    {"dump",         required_argument, 0, 'd'},
    {"status",       no_argument,       0,  1 },
    {"live-dump",    no_argument,       0,  2 },
    {"limit",        no_argument,       0,  3 },
    {"help",         no_argument,       0, 'h'},
    {0,0,0,0}
  };

  int opt, idx;
  while ((opt = getopt_long(argc, argv, "f:t:w:c:p:s:l:d:h", opts, &idx)) != -1)
  {
    switch (opt) {
      case 'f': strncpy(workload_filename, optarg, sizeof workload_filename - 1); break;
      case 't': test_timeout_sec = atoi(optarg);  break;
      case 'w': work_sim_us      = atoi(optarg);  break;
      case 'c': num_clients      = atoi(optarg);  break;
      case 'p': num_puppets      = atoi(optarg);  break;
      case 's': sample_period    = 1 << atoi(optarg); break;
      case 'l': strncpy(log_filename,  optarg, sizeof log_filename - 1); break;
      case 'd': strncpy(dump_filename, optarg, sizeof dump_filename - 1); break;
      case  1 : status_updates = true;  break;
      case  2 : live_dump      = true;  break;
      case  3 : limit_client   = true;  break;
      case 'h':
      default:  fputs(usage, stderr); exit(0);
    }
  }

  /* sanity checks */
  if (test_timeout_sec <= 0 || work_sim_us < 0 ||
    num_clients <= 0   || num_puppets <= 0) {
    FATAL("Invalid argument value\n");
  }

  if (workload_filename[0] == '\0') {
    FATAL("Workload not provided\n");
  }

  if (log_filename[0] == '\0') {
    WARN("Logging to a file is disabled");
    if (!live_dump && !dump_filename[0]) sample_period = 0;
  }

  if ((live_dump || dump_filename[0]) && sample_period == 0) {
    FATAL("Dumping requires sample_period > 0");
  }

}

/*
Main
*/
int main(int argc, char *argv[]) {
  pin_thread_to_core(MAIN_CORE);

  parse_args(argc, argv);
  cpu_freq = measure_cpu_freq();
  work_sim_cycles = (uint64_t)(cpu_freq * (work_sim_us * 1e-6));

  ASSERT(workload_filename[0]);
  workload = parse_workload(workload_filename);

  pmlog_init(workload->num_txns * 6, sample_period, live_dump ? stdout : NULL);
  pmhw_init(num_clients, num_puppets); // Reminder: this creates a scheduler thread

  /*

  Start worker threads
  */

  for (int i = 0; i < num_puppets; ++i) {
    puppets[i].id = i;
    puppets[i].num_completed = 0;
    pthread_create(&puppets[i].thread, NULL, puppet_thread, &puppets[i]);
  }

  /*
  Start client
  */

  pthread_t client;
  pthread_create(&client, NULL, client_thread, NULL); // client_thread starts the timer

  /*
  Wait until we're sure everything is done
  */

  // Every second, check whether everything has finished so we can break out early
  bool done = false;
  bool success = false;
  int prev = -1;
  for (int second = 0; second < (int) test_timeout_sec; second++) {
    int sum = 0;
    for (int i = 0; i < num_puppets; ++i) {
      sum += puppets[i].num_completed;
    }
    if (status_updates) {
      INFO("%d/%d transactions completed", sum, workload->num_txns);
    }
    if (sum == prev) {
      done = true;
      success = false;
      break;
    }
    prev = sum;

    if (sum == workload->num_txns) {
      done = true;
      success = true;
      break;
    }
    sleep(1);
  }

  // Just crash if timed out
  if (!done) {
    ERROR("Timeout after %d seconds", (int) test_timeout_sec);
  } else if (!success) {
    ERROR("Terminated due to no progress");
  } else {
    // Graceful cleanup if possible (otherwise, don't bother)
    pmhw_shutdown();
    atomic_store_explicit(&keep_polling, false, memory_order_relaxed);
    pthread_join(client, NULL);
    for (int i = 0; i < num_puppets; ++i) {
      pthread_join(puppets[i].thread, NULL);
    }
  }

  /*
  Print human-readable timestamp reports
  */
  if (log_filename[0]) {
    FILE *log_file = fopen(log_filename, "wb");
    pmlog_write(log_file);
    fclose(log_file);
  }

  if (dump_filename[0]) {
    FILE *dump_file = fopen(dump_filename, "w");
    pmlog_dump_text(dump_file);
    fclose(dump_file);
  }

  /*
  Don't leak memory
  */
  free(workload);
  pmlog_cleanup();

  return 0;
}

