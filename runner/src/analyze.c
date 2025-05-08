#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "pmlog.h"
#include "pmutils.h"
#include "workload.h"

static bool complain_missing  = false;
static bool complain_order    = true;
static bool complain_conflict = true;

typedef struct {
  uint64_t submit, sched, work, done, cleanup;   /* 0 if missing       */
  uint16_t puppet;                             /* from DONE          */
} timeline_t;

static inline double cycles_to_us(uint64_t c) { return c / cpu_freq * 1e6; }

typedef struct { uint64_t ts_sched, ts_done; int id; } sched_evt_t;

static int compare_sched_evt(const void *a, const void *b) {
  const sched_evt_t *evt_a = (sched_evt_t *)a;
  const sched_evt_t *evt_b = (sched_evt_t *)b;
  if (evt_a->ts_sched < evt_b->ts_sched) return -1;
  if (evt_a->ts_sched > evt_b->ts_sched) return 1;
  return 0;
}

int main(int argc, char *argv[])
{
  if (argc != 5) {
    fprintf(stderr, "Usage: %s transactions.csv log.bin NUM_PUPPETS WORK_SIM_US\n", argv[0]);
    exit(1);
  }

  /* Load all data */

  const char *csv_file  = argv[1];
  const char *log_file  = argv[2];
  int num_puppets       = atoi(argv[3]);
  int work_sim_us       = atoi(argv[4]);

  workload_t *wl = parse_workload(csv_file);
  if (!wl) FATAL("Failed to parse %s", csv_file);

  FILE *lf = fopen(log_file, "rb");
  if (!lf) FATAL("Cannot open %s", log_file);
  double cpu_freq;
  uint64_t base_tsc;
  int num_events = pmlog_read(lf, &cpu_freq, &base_tsc);
  fclose(lf);
  ASSERT(base_tsc != 0);

  INFO("Loaded %d transactions, %d log events, cpu_freq=%.3f GHz",
       wl->num_txns, num_events, cpu_freq / 1e9);

  /* Construct transaction timelines */

  timeline_t *tl = (timeline_t *) calloc(wl->num_txns, sizeof(*tl));
  ASSERT(tl);

  uint64_t first_submit = UINT64_MAX, last_done = 0;
  for (uint32_t i = 0; i < num_events; i++) {
    const pmlog_evt_t *e = &pmlog_evt_buf[i];
    timeline_t *t = &tl[e->txn_id];
    switch (e->kind) {
      case PMLOG_SUBMIT:      t->submit = e->tsc; break;
      case PMLOG_SCHED_READY: t->sched  = e->tsc; break;
      case PMLOG_WORK_RECV:   t->work   = e->tsc; break;
      case PMLOG_DONE:        t->done   = e->tsc; t->puppet = e->aux_data; break;
      case PMLOG_CLEANUP:     t->cleanup = e->tsc; break;
      default: FATAL("Unexpected log kind");
    }
  }

  /* Record first_submit and last_done. Also complain about incomplete transactions.  */

  int missing = 0, order_err = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].submit || !tl[i].sched || !tl[i].work || !tl[i].done || !tl[i].cleanup) {
      if (complain_missing) {
        if (missing < 10) ERROR("Completeness violation: txn_id=%d", i);
        if (missing == 10) INFO("Further completeness violations omitted");
      }
      missing++;
      continue;
    }

    if (!(tl[i].submit <= tl[i].sched && tl[i].sched <= tl[i].work
      && tl[i].work <= tl[i].done && tl[i].done <= tl[i].cleanup)) {
      if (complain_order) {
        if (order_err < 10) ERROR("Ordering violation: txn_id=%d", i);
        if (order_err == 10) INFO("Further ordering violations will be omitted.");
      }
      order_err++;
    }

    if (tl[i].submit < first_submit) first_submit = tl[i].submit;
    if (tl[i].done   > last_done)    last_done    = tl[i].done;
  }

  if (!complain_missing) {
    WARN("Checks for missing transactions are omitted.");
  } else if (missing) {
    ERROR("%d / %d transactions incomplete in log", missing, wl->num_txns);
  } else {
    INFO("All %d transactions are complete.", wl->num_txns);
  }

  if (!complain_order) {
    WARN("Checks for mis-ordered transactions are omitted.");
  } else if (order_err) {
    ERROR("Found %d ordering violations", order_err);
  } else {
    INFO("All available transactions are correctly ordered.", wl->num_txns);
  }

  if (!complain_conflict) {
    // Very expensive operation. If we aren't gonna complain, just skip entirely.
    WARN("Checks for conflicting transactions are omitted.");

  } else {
    sched_evt_t *sched = (sched_evt_t *) malloc(wl->num_txns * sizeof(*sched));
    int sched_cnt = 0;

    // Create an array of interesting transactions sorted by scheduled time
    for (int i = 0; i < wl->num_txns; i++) {
      if (!tl[i].sched || !tl[i].done) continue; 
      if (tl[i].submit > tl[i].sched || tl[i].sched > tl[i].done) continue;
      sched[sched_cnt++] = (sched_evt_t){ tl[i].sched, tl[i].done, i };
    }
    qsort(sched, sched_cnt, sizeof(sched_evt_t), &compare_sched_evt);

    // We'll maintain a list of active transactions
    int active_ids[MAX_ACTIVE_PER_PUPPET * MAX_PUPPETS];
    int active_cnt = 0;

    // Go through the transactions in order
    int conflicts = 0;
    for (int idx = 0; idx < sched_cnt; idx++) {
      uint64_t now_sched = sched[idx].ts_sched;

      // Remove finished transactions from active list
      for (int w = 0; w < active_cnt; ) {
        int tid = active_ids[w];
        if (tl[tid].done <= now_sched) {
          active_ids[w] = active_ids[--active_cnt];
        }
        else {
          w++;
        }
      }

      // Compare against still‑active txns
      int cur_id = sched[idx].id;
      const txn_t *cur_tx = &wl->txns[cur_id];
      for (int w = 0; w < active_cnt; w++) {
        int other_id = active_ids[w];

        if (check_txn_conflict(cur_tx, &wl->txns[other_id])) {
          if (conflicts < 10) {
            WARN("Conflict: txn %d vs %d (sched %.3f µs)",
                 cur_id, other_id,
                 cycles_to_us(now_sched - base_tsc));
          }
          if (conflicts == 10) {
            WARN("Further conflicts omitted");
          }
          conflicts++;
          break;            /* one conflict is enough to flag */
        }
      }

      /* add current txn to active set */
      if (active_cnt < num_puppets)
        active_ids[active_cnt++] = cur_id;
    }

    if (conflicts) {
      ERROR("%d conflicting pairs detected among scheduled txns", conflicts);
    } else {
      INFO("No conflicting pairs of scheduled transactions.");
    }

    free(sched);
  }

  /*
  Print estimated throughputs
  */
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

  free(tl);
  free(wl);
  pmlog_cleanup();
  return 0;
}

