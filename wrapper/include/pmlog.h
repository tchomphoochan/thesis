#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include "pmhw.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Input queue handling + PCIe latency = from PMLOG_SUBMIT to PMLOG_INPUT_RECV
HW queue, scheduling                = from PMLOG_INPUT_RECV to PMLOG_SCHED_RECV
PCIe latency + queue backpressure   = from PMHW_SCHED_READY to PMHW_WORK_RECV
output queue handling + actual work = from PMLOG_WORK_RECV to PMLOG_DONE
                                      expected to be approx. equal to execution time
note we're not logging the time it takes to report work-done back to the hardware.

handled by users: PMLOG_SUBMIT, PMLOG_WORK_RECV, PMLOG_DONE
handled by puppetmaster hardware/wrapper: PMLOG_INPUT_RECV, PMLOG_SCHED_READY
*/
typedef enum {
  PMLOG_SUBMIT         = 0,  /* client starts trying to submit txn     */
  PMLOG_SCHED_READY    = 1,  /* hardware scheduled the txn             */
  PMLOG_WORK_RECV      = 2,  /* client got txn work request            */
  PMLOG_DONE           = 3,  /* puppet finished processing             */
  PMLOG_CLEANUP        = 4   /* puppet finished processing             */
} pmlog_kind_t;

typedef struct {
  uint64_t tsc;       /* raw timestamp, 0 if none */
  txn_id_t txn_id;
  pmlog_kind_t kind;
  uint64_t aux_data;  /* for PMLOG_DONE: puppet id */
} pmlog_evt_t;

void pmlog_init(int max_num_events, int sample_period, FILE *live_print);
void pmlog_cleanup();
void pmlog_record(txn_id_t txn_id, pmlog_kind_t kind, uint64_t aux_data);

void pmlog_start_timer(double cpu_freq);
void pmlog_write(FILE *f);
int pmlog_read(FILE *f, double *cpu_freq, uint64_t *base_tsc);
void pmlog_dump_text(FILE *f);

extern pmlog_evt_t *pmlog_evt_buf;

#ifdef __cplusplus
}
#endif
