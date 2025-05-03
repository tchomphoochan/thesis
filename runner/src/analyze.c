/*********************************************************************
 * analyze.c  –  minimal consistency‑checker & throughput estimator
 *
 *  Usage:
 *      ./analyze transactions.csv log.bin NUM_PUPPETS WORK_SIM_US
 *
 *********************************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include "pmlog.h"
#include "pmutils.h"
#include "workload.h"

/* --------------------------------------------------------- */
/*              compile‑time toggles                         */
/* --------------------------------------------------------- */
#define ENABLE_CONFLICT_CHECK     0   /* heavy; leave 0 for now  */
#define ENABLE_ORDER_CHECK        1
#define ENABLE_COMPLETENESS_CHECK 1

static int num_events;
static double cpu_freq;
static uint64_t base_tsc;

/* --------------------------------------------------------- */
typedef struct {
  uint64_t submit, sched, work, done, cleanup;   /* 0 if missing       */
  uint16_t puppet;                             /* from DONE          */
} timeline_t;

static inline double cycles_to_us(uint64_t c) { return c / cpu_freq * 1e6; }

/* --------------------------------------------------------- */
static void usage_and_die(const char *argv0)
{
  fprintf(stderr,
          "Usage: %s transactions.csv log.bin NUM_PUPPETS WORK_SIM_US\n", argv0);
  exit(1);
}

/* --------------------------------------------------------- */
int main(int argc, char **argv)
{
  if (argc != 5) usage_and_die(argv[0]);

  const char *csv_file  = argv[1];
  const char *log_file  = argv[2];
  int num_puppets       = atoi(argv[3]);
  int work_sim_us       = atoi(argv[4]);

  workload_t *wl = parse_workload(csv_file);
  if (!wl) FATAL("Failed to parse %s", csv_file);

  FILE *lf = fopen(log_file, "rb");
  if (!lf) FATAL("Cannot open %s", log_file);
  num_events = pmlog_read(lf, &cpu_freq, &base_tsc);
  fclose(lf);

  INFO("Loaded %d transactions, %d log events, cpu_freq=%.3f MHz",
       wl->num_txns, num_events, cpu_freq / 1e6);

  timeline_t *tl = calloc(wl->num_txns, sizeof(*tl));
  ASSERT(tl);

  uint64_t first_submit = UINT64_MAX, last_done = 0;

  /* --------------- build per‑txn timelines ---------------- */
  for (uint32_t i = 0; i < num_events; i++) {
    const pmlog_evt_t *e = &pmlog_evt_buf[i];
    timeline_t *t = &tl[e->txn_id];
    switch (e->kind) {
      case PMLOG_SUBMIT:      t->submit = e->tsc; break;
      case PMLOG_SCHED_READY: t->sched  = e->tsc; break;
      case PMLOG_WORK_RECV:   t->work   = e->tsc; break;
      case PMLOG_DONE:        t->done   = e->tsc; t->puppet = e->aux_data; break;
      case PMLOG_CLEANUP:     t->cleanup = e->tsc; break;
      default: break;
    }
  }

  /* --------------- completeness & order checks ------------ */
#if ENABLE_COMPLETENESS_CHECK
  int missing = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].submit || !tl[i].sched || !tl[i].work || !tl[i].done || !tl[i].cleanup) {
      if (missing < 10) WARN("Completeness violation: txn_id=%d", i);
      if (missing == 10) WARN("Further completeness violations omitted");
      missing++;
      continue;
    }
    if (tl[i].submit < first_submit) first_submit = tl[i].submit;
    if (tl[i].done   > last_done)    last_done    = tl[i].done;
  }
  if (missing) {
    ERROR("%d / %d transactions incomplete in log", missing, wl->num_txns);
  }
#endif

#if ENABLE_ORDER_CHECK
  int order_err = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    bool complete = tl[i].submit && tl[i].sched && tl[i].work && tl[i].done && tl[i].cleanup;
    if (complete && !(tl[i].submit <= tl[i].sched && tl[i].sched <= tl[i].work
          && tl[i].work <= tl[i].done && tl[i].done <= tl[i].cleanup)) {
      if (order_err < 10) WARN("Ordering violation: txn_id=%d", i);
      if (order_err == 10) WARN("Further ordering violations omitted");
      order_err++;
    }
  }
  if (order_err) ERROR("%d ordering violations", order_err);
#endif

#if ENABLE_CONFLICT_CHECK
  /* TODO later – uses object array & overlap window */
#endif

  /* --------------- throughput estimate -------------------- */
  double elapsed_s = (last_done - first_submit) / cpu_freq;
  double throughput = (double)wl->num_txns / elapsed_s;

  printf("Summary\n"
         "========\n"
         "Txns           : %d\n"
         "Puppets        : %d\n"
         "Sim work (µs)  : %d\n"
         "Runtime (s)    : %.6f\n"
         "Throughput tx/s: %.2f\n",
         wl->num_txns, num_puppets, work_sim_us, elapsed_s, throughput);

  /* --------------- TODO future work: histograms, windowing -------- */

  free(tl);
  free(wl);      /* parse_workload malloc’d single block */
  pmlog_cleanup();
  return 0;
}

