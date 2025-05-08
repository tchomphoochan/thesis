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
static const int num_throughput_windows = 50;

// Outlier removal configuration
static const double lower_percentile_cutoff = 0.01; // Remove bottom 1%
static const double upper_percentile_cutoff = 0.99; // Remove top 1%

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

// Compare function for unsigned 64-bit integers (for latency sorting)
static int compare_uint64(const void *a, const void *b) {
  const uint64_t *ua = (const uint64_t *)a;
  const uint64_t *ub = (const uint64_t *)b;
  if (*ua < *ub) return -1;
  if (*ua > *ub) return 1;
  return 0;
}

// Structure to hold histogram data
typedef struct {
  double center;  // Center of the bucket in seconds
  int count;      // Count of values in this bucket
  double cdf;     // Cumulative distribution function value
} histogram_bucket_t;

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

  INFO("Loaded %d transactions, %d log events, cpu_freq=%.3f GHz",
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

  /*
  Compute execution duration and window sizes
  */
  uint64_t duration_cycles = last_done - first_submit;
  uint64_t window_cycles = duration_cycles / num_throughput_windows;
  double duration_seconds = (double)duration_cycles / cpu_freq;
  double window_seconds = (double)window_cycles / cpu_freq;

  INFO("Execution duration: %.6f seconds", duration_seconds);
  INFO("Window size: %.6f seconds", window_seconds);

  /*
  Compute windowed throughput (all completely logged transactions)
  */
  // Initialize window counters
  int *windowed_submits  = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_scheds   = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_recvs    = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_dones    = (int*) calloc(num_throughput_windows, sizeof(int));
  int *windowed_cleanups = (int*) calloc(num_throughput_windows, sizeof(int));

  // Count events per window
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].complete) continue;

    // Calculate window indices for each event stage
    int submit_window = (tl[i].submit - first_submit) / window_cycles;
    int sched_window = (tl[i].sched - first_submit) / window_cycles;
    int recv_window = (tl[i].work - first_submit) / window_cycles;
    int done_window = (tl[i].done - first_submit) / window_cycles;
    int cleanup_window = (tl[i].cleanup - first_submit) / window_cycles;

    // Bounds checking
    if (submit_window >= 0 && submit_window < num_throughput_windows)
      windowed_submits[submit_window]++;
    if (sched_window >= 0 && sched_window < num_throughput_windows)
      windowed_scheds[sched_window]++;
    if (recv_window >= 0 && recv_window < num_throughput_windows)
      windowed_recvs[recv_window]++;
    if (done_window >= 0 && done_window < num_throughput_windows)
      windowed_dones[done_window]++;
    if (cleanup_window >= 0 && cleanup_window < num_throughput_windows)
      windowed_cleanups[cleanup_window]++;
  }

  // Prepare arrays for output
  double *throughput_x = (double*) malloc(num_throughput_windows * sizeof(double)); // Time in seconds
  double *submit_y = (double*) malloc(num_throughput_windows * sizeof(double));     // Throughput in txn/s
  double *sched_y = (double*) malloc(num_throughput_windows * sizeof(double));
  double *recv_y = (double*) malloc(num_throughput_windows * sizeof(double));
  double *done_y = (double*) malloc(num_throughput_windows * sizeof(double));
  double *cleanup_y = (double*) malloc(num_throughput_windows * sizeof(double));

  // Calculate throughput for each window
  double extrapolate_factor = (double)wl->num_txns / complete_txns;
  INFO("Extrapolation factor: %.2f (logged %d of %d txns)", 
       extrapolate_factor, complete_txns, wl->num_txns);

  for (int i = 0; i < num_throughput_windows; i++) {
    // Convert window index to seconds for x-axis
    throughput_x[i] = (i + 0.5) * window_seconds; // Use midpoint of window

    // Calculate throughput as txn/s with extrapolation
    submit_y[i] = (windowed_submits[i] * extrapolate_factor) / window_seconds;
    sched_y[i] = (windowed_scheds[i] * extrapolate_factor) / window_seconds;
    recv_y[i] = (windowed_recvs[i] * extrapolate_factor) / window_seconds;
    done_y[i] = (windowed_dones[i] * extrapolate_factor) / window_seconds;
    cleanup_y[i] = (windowed_cleanups[i] * extrapolate_factor) / window_seconds;
  }

  /*
  Compute latencies (all complete and ordered transactions within time range of interest).
  */

  // Define range of interest: exclude warmup and cooldown periods
  uint64_t first_included = first_submit + (uint64_t)(duration_cycles * fraction_warmup_time);
  uint64_t last_included = last_done - (uint64_t)(duration_cycles * fraction_cooldown_time);

  INFO("Excluding warmup (first %.1f%%) and cooldown (last %.1f%%) periods",
       fraction_warmup_time * 100, fraction_cooldown_time * 100);

  // First pass: count how many transactions fall within our range of interest
  int latency_count = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].complete || !tl[i].ordered) continue;
    if (tl[i].submit < first_included || tl[i].done > last_included) continue;
    latency_count++;
  }

  INFO("Found %d transactions for latency analysis (%.1f%% of ordered transactions)",
       latency_count, 100.0 * latency_count / ordered_txns);

  // Allocate memory for latency data
  uint64_t *submit_done_latencies = (uint64_t*) malloc(latency_count * sizeof(uint64_t));
  uint64_t *submit_sched_latencies = (uint64_t*) malloc(latency_count * sizeof(uint64_t));
  uint64_t *sched_recv_latencies = (uint64_t*) malloc(latency_count * sizeof(uint64_t));
  uint64_t *recv_done_latencies = (uint64_t*) malloc(latency_count * sizeof(uint64_t));
  uint64_t *done_cleanup_latencies = (uint64_t*) malloc(latency_count * sizeof(uint64_t));

  // Second pass: collect latency data
  int idx = 0;
  for (int i = 0; i < wl->num_txns; i++) {
    if (!tl[i].complete || !tl[i].ordered) continue;
    if (tl[i].submit < first_included || tl[i].done > last_included) continue;

    submit_done_latencies[idx] = tl[i].done - tl[i].submit;
    submit_sched_latencies[idx] = tl[i].sched - tl[i].submit;
    sched_recv_latencies[idx] = tl[i].work - tl[i].sched;
    recv_done_latencies[idx] = tl[i].done - tl[i].work;
    done_cleanup_latencies[idx] = tl[i].cleanup - tl[i].done;
    idx++;
  }

  ASSERT(idx == latency_count);

  /*
  Process latencies with outlier removal
  */

  // Sort latencies for outlier removal
  qsort(submit_done_latencies, latency_count, sizeof(uint64_t), compare_uint64);
  qsort(submit_sched_latencies, latency_count, sizeof(uint64_t), compare_uint64);
  qsort(sched_recv_latencies, latency_count, sizeof(uint64_t), compare_uint64);
  qsort(recv_done_latencies, latency_count, sizeof(uint64_t), compare_uint64);
  qsort(done_cleanup_latencies, latency_count, sizeof(uint64_t), compare_uint64);

  // Calculate outlier cutoff indices
  int lower_idx = (int)(latency_count * lower_percentile_cutoff);
  int upper_idx = (int)(latency_count * upper_percentile_cutoff) - 1;
  int filtered_count = upper_idx - lower_idx + 1;

  INFO("Removing outliers: %.1f%% low, %.1f%% high (keeping %d txns)",
       lower_percentile_cutoff * 100, (1.0 - upper_percentile_cutoff) * 100, filtered_count);

  /*
  Generate histograms from filtered latency data
  */

  // Function to find min/max latencies for each stage
#define FIND_MINMAX(arr, min_var, max_var) do { \
  min_var = arr[lower_idx]; \
  max_var = arr[upper_idx]; \
} while(0)

  // Find min/max latencies for each stage
  uint64_t min_e2e, max_e2e;
  uint64_t min_submit_sched, max_submit_sched;
  uint64_t min_sched_recv, max_sched_recv;
  uint64_t min_recv_done, max_recv_done;
  uint64_t min_done_cleanup, max_done_cleanup;

  FIND_MINMAX(submit_done_latencies, min_e2e, max_e2e);
  FIND_MINMAX(submit_sched_latencies, min_submit_sched, max_submit_sched);
  FIND_MINMAX(sched_recv_latencies, min_sched_recv, max_sched_recv);
  FIND_MINMAX(recv_done_latencies, min_recv_done, max_recv_done);
  FIND_MINMAX(done_cleanup_latencies, min_done_cleanup, max_done_cleanup);

  // Convert min/max to seconds
  double min_e2e_s = min_e2e / cpu_freq;
  double max_e2e_s = max_e2e / cpu_freq;
  double min_submit_sched_s = min_submit_sched / cpu_freq;
  double max_submit_sched_s = max_submit_sched / cpu_freq;
  double min_sched_recv_s = min_sched_recv / cpu_freq;
  double max_sched_recv_s = max_sched_recv / cpu_freq;
  double min_recv_done_s = min_recv_done / cpu_freq;
  double max_recv_done_s = max_recv_done / cpu_freq;
  double min_done_cleanup_s = min_done_cleanup / cpu_freq;
  double max_done_cleanup_s = max_done_cleanup / cpu_freq;

  INFO("End-to-end latency range: %.6f - %.6f seconds", min_e2e_s, max_e2e_s);

  // Create histogram buckets for each latency type
  histogram_bucket_t *e2e_hist = (histogram_bucket_t*) calloc(num_buckets, sizeof(histogram_bucket_t));
  histogram_bucket_t *submit_sched_hist = (histogram_bucket_t*) calloc(num_buckets, sizeof(histogram_bucket_t));
  histogram_bucket_t *sched_recv_hist = (histogram_bucket_t*) calloc(num_buckets, sizeof(histogram_bucket_t));
  histogram_bucket_t *recv_done_hist = (histogram_bucket_t*) calloc(num_buckets, sizeof(histogram_bucket_t));
  histogram_bucket_t *done_cleanup_hist = (histogram_bucket_t*) calloc(num_buckets, sizeof(histogram_bucket_t));

  // Helper function to initialize histogram buckets
  void init_histogram(histogram_bucket_t *hist, double min_val, double max_val) {
    double bucket_width = (max_val - min_val) / num_buckets;
    for (int i = 0; i < num_buckets; i++) {
      hist[i].center = min_val + bucket_width * (i + 0.5);
      hist[i].count = 0;
      hist[i].cdf = 0.0;
    }
  }

  // Helper function to populate histogram
  void populate_histogram(histogram_bucket_t *hist, uint64_t *latencies, 
                          double min_val, double max_val, int start_idx, int end_idx) {
    double bucket_width = (max_val - min_val) / num_buckets;
    for (int i = start_idx; i <= end_idx; i++) {
      double val_s = latencies[i] / cpu_freq;
      int bucket = (val_s - min_val) / bucket_width;
      if (bucket < 0) bucket = 0;
      if (bucket >= num_buckets) bucket = num_buckets - 1;
      hist[bucket].count++;
    }

    // Calculate CDF
    int total = 0;
    for (int i = 0; i < num_buckets; i++) {
      total += hist[i].count;
    }

    int cumulative = 0;
    for (int i = 0; i < num_buckets; i++) {
      cumulative += hist[i].count;
      hist[i].cdf = (double)cumulative / total;
    }
  }

  // Initialize and populate histograms
  init_histogram(e2e_hist, min_e2e_s, max_e2e_s);
  init_histogram(submit_sched_hist, min_submit_sched_s, max_submit_sched_s);
  init_histogram(sched_recv_hist, min_sched_recv_s, max_sched_recv_s);
  init_histogram(recv_done_hist, min_recv_done_s, max_recv_done_s);
  init_histogram(done_cleanup_hist, min_done_cleanup_s, max_done_cleanup_s);

  populate_histogram(e2e_hist, submit_done_latencies, min_e2e_s, max_e2e_s, lower_idx, upper_idx);
  populate_histogram(submit_sched_hist, submit_sched_latencies, min_submit_sched_s, max_submit_sched_s, lower_idx, upper_idx);
  populate_histogram(sched_recv_hist, sched_recv_latencies, min_sched_recv_s, max_sched_recv_s, lower_idx, upper_idx);
  populate_histogram(recv_done_hist, recv_done_latencies, min_recv_done_s, max_recv_done_s, lower_idx, upper_idx);
  populate_histogram(done_cleanup_hist, done_cleanup_latencies, min_done_cleanup_s, max_done_cleanup_s, lower_idx, upper_idx);

  /*
  Calculate average throughput for steady-state period (excluding warmup/cooldown)
  */
  double steady_state_duration = (last_included - first_included) / cpu_freq;
  int steady_state_txns = latency_count;
  double average_throughput = (steady_state_txns * extrapolate_factor) / steady_state_duration;

  INFO("Steady-state throughput: %.2f txn/s over %.6f seconds", 
       average_throughput, steady_state_duration);

  /*
  Output binary file for graphing
  */
  FILE *out = fopen("analyzed.bin", "wb");
  if (!out) FATAL("Cannot open analyzed.bin for writing");

  // Write header information
  fwrite(&wl->num_txns, sizeof(int), 1, out);
  fwrite(&complete_txns, sizeof(int), 1, out);
  fwrite(&filtered_count, sizeof(int), 1, out);
  fwrite(&num_puppets, sizeof(int), 1, out);
  fwrite(&average_throughput, sizeof(double), 1, out);

  // Write throughput window information
  fwrite(&num_throughput_windows, sizeof(int), 1, out);
  fwrite(&window_seconds, sizeof(double), 1, out);

  // Write windowed throughput data
  for (int i = 0; i < num_throughput_windows; i++) {
    fwrite(&throughput_x[i], sizeof(double), 1, out);
    fwrite(&submit_y[i], sizeof(double), 1, out);
  }

  for (int i = 0; i < num_throughput_windows; i++) {
    fwrite(&throughput_x[i], sizeof(double), 1, out);
    fwrite(&sched_y[i], sizeof(double), 1, out);
  }

  for (int i = 0; i < num_throughput_windows; i++) {
    fwrite(&throughput_x[i], sizeof(double), 1, out);
    fwrite(&recv_y[i], sizeof(double), 1, out);
  }

  for (int i = 0; i < num_throughput_windows; i++) {
    fwrite(&throughput_x[i], sizeof(double), 1, out);
    fwrite(&done_y[i], sizeof(double), 1, out);
  }

  for (int i = 0; i < num_throughput_windows; i++) {
    fwrite(&throughput_x[i], sizeof(double), 1, out);
    fwrite(&cleanup_y[i], sizeof(double), 1, out);
  }

  // Write histogram information
  fwrite(&num_buckets, sizeof(int), 1, out);

  // Write end-to-end latency histogram
  for (int i = 0; i < num_buckets; i++) {
    fwrite(&e2e_hist[i].center, sizeof(double), 1, out);
    fwrite(&e2e_hist[i].count, sizeof(int), 1, out);
    fwrite(&e2e_hist[i].cdf, sizeof(double), 1, out);
  }

  // Write submit->sched latency histogram
  for (int i = 0; i < num_buckets; i++) {
    fwrite(&submit_sched_hist[i].center, sizeof(double), 1, out);
    fwrite(&submit_sched_hist[i].count, sizeof(int), 1, out);
    fwrite(&submit_sched_hist[i].cdf, sizeof(double), 1, out);
  }

  // Write sched->work latency histogram
  for (int i = 0; i < num_buckets; i++) {
    fwrite(&sched_recv_hist[i].center, sizeof(double), 1, out);
    fwrite(&sched_recv_hist[i].count, sizeof(int), 1, out);
    fwrite(&sched_recv_hist[i].cdf, sizeof(double), 1, out);
  }

  // Write work->done latency histogram
  for (int i = 0; i < num_buckets; i++) {
    fwrite(&recv_done_hist[i].center, sizeof(double), 1, out);
    fwrite(&recv_done_hist[i].count, sizeof(int), 1, out);
    fwrite(&recv_done_hist[i].cdf, sizeof(double), 1, out);
  }

  // Write done->cleanup latency histogram
  for (int i = 0; i < num_buckets; i++) {
    fwrite(&done_cleanup_hist[i].center, sizeof(double), 1, out);
    fwrite(&done_cleanup_hist[i].count, sizeof(int), 1, out);
    fwrite(&done_cleanup_hist[i].cdf, sizeof(double), 1, out);
  }

  fclose(out);
  INFO("Binary data written to out.bin");

  /*
  Print estimated throughputs
  */
  double elapsed_s = (last_done - first_submit) / cpu_freq; 
  double raw_throughput = (double)wl->num_txns / elapsed_s;

  printf("Summary\n"
         "========\n"
         "Txns           : %d\n"
         "Puppets        : %d\n"
         "Sim work (µs)  : %d\n"
         "Runtime (s)    : %.6f\n"
         "Throughput tx/s: %.2f (raw), %.2f (steady-state)\n",
         wl->num_txns, num_puppets, work_sim_us, elapsed_s, 
         raw_throughput, average_throughput);

  // Clean up
  free(windowed_submits);
  free(windowed_scheds);
  free(windowed_recvs);
  free(windowed_dones);
  free(windowed_cleanups);

  free(throughput_x);
  free(submit_y);
  free(sched_y);
  free(recv_y);
  free(done_y);
  free(cleanup_y);

  free(submit_done_latencies);
  free(submit_sched_latencies);
  free(sched_recv_latencies);
  free(recv_done_latencies);
  free(done_cleanup_latencies);

  free(e2e_hist);
  free(submit_sched_hist);
  free(sched_recv_hist);
  free(recv_done_hist);
  free(done_cleanup_hist);

  free(tl);
  free(wl);
  pmlog_cleanup();

  return 0;
}
