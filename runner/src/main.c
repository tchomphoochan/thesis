#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>

#include "pmhw.h"

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
#define CHECK_OK(retcode) \
  do { \
    pmhw_retval_t _r = (retcode); \
    if (_r != PMHW_OK) { \
      FATAL("pmhw_retval_t returned %d but expected %d", (int)_r, (int)PMHW_OK); \
    } \
  } while (0)

#define CHECK_EXPECTED(retcode, expected) \
  do { \
    pmhw_retval_t _r = (retcode); \
    pmhw_retval_t _e = (expected); \
    if (_r != _e) { \
      FATAL("pmhw_retval_t returned %d but expected %d", (int)_r, (int)_e); \
    } \
  } while (0)

/*
Global state
*/
volatile int keep_polling = 1;
struct timespec start_time;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
Time utilities
*/
void get_elapsed(struct timespec *out) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  out->tv_sec = now.tv_sec - start_time.tv_sec;
  out->tv_nsec = now.tv_nsec - start_time.tv_nsec;
  if (out->tv_nsec < 0) {
    out->tv_sec -= 1;
    out->tv_nsec += 1000000000;
  }
}

/*
Polling thread
*/
void *poller_thread(void *arg) {
  while (keep_polling) {
    int ret = 0;
    pmhw_retval_t status = pmhw_poll_scheduled(&ret);
    if (status != PMHW_OK) {
      FATAL("pmhw_poll_scheduled failed with status %d", status);
    }

    if (ret) {
      struct timespec elapsed;
      get_elapsed(&elapsed);

      pthread_mutex_lock(&print_mutex);
      printf("[%+.6f] scheduled txn id=%d\n",
             elapsed.tv_sec + elapsed.tv_nsec / 1e9, ret);
      fflush(stdout);
      pthread_mutex_unlock(&print_mutex);
    } else {
      sched_yield();
    }
  }
  return NULL;
}

/*
Parse CSV transaction
Format: aux_data(int), objid(int), write(1/0), objid(int), write(1/0), ...
*/
void parse_txn(pmhw_txn_t *txn, int id, const char *buf) {
  txn->transactionId = id;
  txn->numReadObjs = 0;
  txn->numWriteObjs = 0;

  const char *p = buf;
  int objid, writeflag;

  // Parse aux_data
  uint64_t aux_data;
  if (sscanf(p, "%lu", &aux_data) != 1) {
    FATAL("Failed to parse aux_data in transaction line: '%s'", buf);
  }
  txn->auxData = aux_data;

  while (*p && *p != ',') p++;
  if (*p == ',') p++;

  // Parse (objid, writeflag) pairs
  while (*p) {
    if (sscanf(p, "%d", &objid) != 1) {
      FATAL("Failed to parse object ID in transaction line: '%s'", buf);
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    if (sscanf(p, "%d", &writeflag) != 1) {
      FATAL("Failed to parse write flag in transaction line: '%s'", buf);
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    if (txn->numReadObjs + txn->numWriteObjs >= PMHW_MAX_TXN_TOTAL_OBJS) {
      FATAL("Too many objects in transaction %d", id);
    }

    if (writeflag) {
      if (txn->numWriteObjs >= PMHW_MAX_TXN_WRITE_OBJS) {
        FATAL("Too many write objects in transaction %d", id);
      }
      txn->writeObjIds[txn->numWriteObjs++] = (uint64_t)objid;
    } else {
      if (txn->numReadObjs >= PMHW_MAX_TXN_READ_OBJS) {
        FATAL("Too many read objects in transaction %d", id);
      }
      txn->readObjIds[txn->numReadObjs++] = (uint64_t)objid;
    }
  }
}

/*
Main
*/
int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <file> <period>\n", argv[0]);
    fprintf(stderr, "  <file>  : CSV file containing transactions to simulate\n");
    fprintf(stderr, "  <period>: Simulated puppets' clock period\n");
    exit(1);
  }

  // Set up the hardware
  CHECK_OK(pmhw_reset());

  pmhw_config_t config;
  CHECK_OK(pmhw_get_config(&config));
  config.useSimulatedPuppets = false;
  config.useSimulatedTxnDriver = false;
  config.simulatedPuppetsClockPeriod = atoi(argv[2]);
  CHECK_EXPECTED(pmhw_set_config(&config), PMHW_PARTIAL);

  printf("== Puppetmaster Configuration ==\n");
  printf("%30s: %d\n", "logNumberRenamerThreads", config.logNumberRenamerThreads);
  printf("%30s: %d\n", "logNumberShards", config.logNumberShards);
  printf("%30s: %d\n", "logSizeShard", config.logSizeShard);
  printf("%30s: %d\n", "logNumberHashes", config.logNumberHashes);
  printf("%30s: %d\n", "logNumberComparators", config.logNumberComparators);
  printf("%30s: %d\n", "logNumberSchedulingRounds", config.logNumberSchedulingRounds);
  printf("%30s: %d\n", "logNumberPuppets", config.logNumberPuppets);
  printf("%30s: %d\n", "numberAddressOffsetBits", config.numberAddressOffsetBits);
  printf("%30s: %d\n", "logSizeRenamerBuffer", config.logSizeRenamerBuffer);
  printf("%30s: %d\n", "useSimulatedTxnDriver", config.useSimulatedTxnDriver);
  printf("%30s: %d\n", "useSimulatedPuppets", config.useSimulatedPuppets);
  printf("%30s: %d\n", "simulatedPuppetsClockPeriod", config.simulatedPuppetsClockPeriod);

  // Start the timer BEFORE threads
  clock_gettime(CLOCK_MONOTONIC, &start_time);

  // Now start poller thread
  pthread_t poller;
  if (pthread_create(&poller, NULL, poller_thread, NULL) != 0) {
    FATAL("Failed to create poller thread");
  }

  // Load and submit transactions
  pmhw_txn_t txn;
  int id = 0;
  int buf_size = 1000;
  char buf[buf_size];
  FILE *txn_file = fopen(argv[1], "r");
  if (!txn_file) {
    FATAL("Could not open file '%s'", argv[1]);
  }
  while (fgets(buf, buf_size, txn_file)) {
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf) == 0) continue;
    parse_txn(&txn, id, buf);

    struct timespec elapsed;
    get_elapsed(&elapsed);

    pthread_mutex_lock(&print_mutex);
    printf("[%+.6f] submitted txn id=%d aux=%lu R(", 
           elapsed.tv_sec + elapsed.tv_nsec / 1e9, txn.transactionId, txn.auxData);
    for (int i = 0; i < txn.numReadObjs; ++i) {
      printf("%lu", txn.readObjIds[i]);
      if (i != txn.numReadObjs - 1) printf(",");
    }
    printf(") W(");
    for (int i = 0; i < txn.numWriteObjs; ++i) {
      printf("%lu", txn.writeObjIds[i]);
      if (i != txn.numWriteObjs - 1) printf(",");
    }
    printf(")\n");
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);

    CHECK_OK(pmhw_schedule(&txn));
    id++;
  }
  fclose(txn_file);

  // Let the poller thread run a little longer
  sleep(1);

  // Stop poller
  keep_polling = 0;
  pthread_join(poller, NULL);

  return 0;
}

