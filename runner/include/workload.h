#pragma once

#include "pmhw.h"

typedef struct {
  int num_txns;
  txn_t txns[];
} workload_t;

workload_t *parse_workload(const char *filename);
