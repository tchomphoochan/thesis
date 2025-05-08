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

static const bool complain_missing  = false;
static const bool complain_order    = true;
static const bool complain_conflict = true;

static const int num_buckets = 64;
static const double fraction_warmup_time = 0.1;
static const double fraction_cooldown_time = 0.1;
static const int num_throughput_windows = 100;

typedef struct {
  uint64_t submit, sched, work, done, cleanup; // 0 is missing
  bool complete;
  bool ordered;
  uint16_t puppet;
} timeline_t;

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

  /*
  Load all data
  */

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

  /*
  Construct transaction timelines
  */

  timeline_t *tl = (timeline_t *) calloc(wl->num_txns, sizeof(timeline_t));
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

  /*
  Record first_submit and last_done. Also complain about incomplete transactions.
  */

  int missing = 0, order_err = 0;
  int complete_txns = 0, ordered_txns = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].submit || !tl[i].sched || !tl[i].work || !tl[i].done || !tl[i].cleanup) {
      if (complain_missing) {
        if (missing < 10) ERROR("Completeness violation: txn_id=%d", i);
        if (missing == 10) INFO("Further completeness violations omitted");
      }
      missing++;
      continue;
    }
    tl[i].complete = true;
    complete_txns++;

    if (!(tl[i].submit <= tl[i].sched && tl[i].sched <= tl[i].work
      && tl[i].work <= tl[i].done && tl[i].done <= tl[i].cleanup)) {
      if (complain_order) {
        if (order_err < 10) ERROR("Ordering violation: txn_id=%d", i);
        if (order_err == 10) INFO("Further ordering violations will be omitted.");
      }
      order_err++;
    } else {
      tl[i].ordered = true;
      ordered_txns++;
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

  /*
  Conflict checks
  */

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
            WARN("Conflict: txn %d vs %d", cur_id, other_id);
          }
          if (conflicts == 10) {
            WARN("Further conflicts omitted");
          }
          conflicts++;
          break;            /* one conflict is enough to flag */
        }
      }

      // Add current txn to active set
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

  // binary file format: start
  // 
  // - int: total number of transactions
  // - int: number of logged transactions
  // - int: number of histogram-included transactions (important for histogram y axis)
  // - int: num puppets
  // - double: extrapolated overall throughput txn/s (with warmup/cooldown removed)
  //
  // windowed throughput. all logged transactions are used for calculation (n = logged). extrapolate to total txns.
  // - int: number of throughput windows
  // - double: throughput window size in seconds (to print it on the graph)
  // - n pairs of (64-bit double in seconds x, 64-bit double throughput/s y) for submit
  // - n pairs of (64-bit double in seconds x, 64-bit double throughput/s y) for ready
  // - n pairs of (64-bit double in seconds x, 64-bit double throughput/s y) for work recv
  // - n pairs of (64-bit double in seconds x, 64-bit double throughput/s y) for done
  // - n pairs of (64-bit double in seconds x, 64-bit double throughput/s y) for cleanup
  //
  // end-to-end latency data. all included transactions are used for histogram. (n = included)
  // - int: num histogram buckets
  // - pairs of (64-bit double in seconds for histogram center x, int: count y, double: CDF y2)
  //    - n for e2e (submit->done)
  //    - n for submit->sched
  //    - n for sched->work
  //    - n for work->done
  //    - n for done->cleanup
  //
  // binary file format: end

  FILE *out = fopen("out.bin", "wb");

  /*
  Compute windowed throughput (all completely logged transactions)
  */
  uint64_t duration_cycles = last_done - first_submit;
  uint64_t window_cycles = duration_cycles / num_throughput_windows;
  // counters
  int *windowed_submits  = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_scheds   = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_recvs    = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_dones    = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_cleanups = (int*) calloc(num_throughput_windows, sizeof(int));

  // TODO: go through tl with completed, increment window


  /*
  Compute latencies (all complete and ordered transactions within time range of interest).
  */

  // range of interest:
  // a transaction is included if submit >= first_included and done <= last_included.
  uint64_t first_included = first_submit + duration_cycles*fraction_warmup_time;
  uint64_t last_included = last_done - duration_cycles*fraction_cooldown_time;

  int latency_count = 0;
  uint64_t *submit_done_latencies = (uint64_t*) calloc(wl->num_txns, sizeof(uint64_t));
  uint64_t *submit_sched_latencies = (uint64_t*) calloc(wl->num_txns, sizeof(uint64_t));
  uint64_t *sched_recv_latencies   = (uint64_t*) calloc(wl->num_txns, sizeof(uint64_t));
  uint64_t *recv_done_latencies    = (uint64_t*) calloc(wl->num_txns, sizeof(uint64_t));
  uint64_t *done_cleanup_latencies = (uint64_t*) calloc(wl->num_txns, sizeof(uint64_t));

  // TODO: for ordered_txns included in the range, add to latency data
  // TODO: also figure out how to calculate average throughput within range of interest
  //       (maybe count num of txns in range, multiply by extrapolate, divided by duration?)


  /*
  Output binary file for graphing
  */

  // throughput extrapolation
  double extrapolate_factor = (double) wl->num_txns / complete_txns;
  double average_throughput; // TODO

  fwrite(&wl->num_txns, sizeof(int), 1, out);
  fwrite(&complete_txns, sizeof(int), 1, out);
  fwrite(&latency_count, sizeof(int), 1, out);
  fwrite(&num_puppets, sizeof(int), 1, out);
  fwrite(&average_throughput, sizeof(int), 1, out);

  // output windowed throughputs
  fwrite(&num_throughput_windows, sizeof(int), 1, out);
  double window_seconds; // TODO
  fwrite(&window_seconds, sizeof(double), 1, out);
  // TODO: use the counts above, but recalculate as txn/s. also recalculate timestamp cycles into seconds. output to file.
  // output for each kind of throughput


  // TODO: output latency histogram+cdf


  fclose(out);

  /*
  Print estimated throughputs
  */
  double elapsed_s = (last_done - first_submit) / cpu_freq; // clearly mark as wallclock time
  double throughput = (double)wl->num_txns / elapsed_s; // TODO: use the better calculated version

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

