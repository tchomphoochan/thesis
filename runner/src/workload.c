#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>

#include "workload.h"
#include "pmhw.h"
#include "pmutils.h"

/*
Parse CSV transaction
*/
static int count_lines(FILE *f) {
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
static void parse_txn(txn_t *txn, int id, const char *buf) {
  txn->id = id;
  txn->num_objs = 0;

  const char *p = buf;
  txn_id_t objid;
  int writeflag;

  uint64_t aux_data;
  if (sscanf(p, "%lu", &aux_data) != 1) {
    FATAL("Failed to parse aux_data");
  }
  txn->aux_data = aux_data;

  while (*p && *p != ',') p++;
  if (*p == ',') p++;

  while (*p) {
    if (sscanf(p, "%lu", &objid) != 1) {
      FATAL("Failed to parse objid");
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    if (sscanf(p, "%d", &writeflag) != 1) {
      FATAL("Failed to parse writeflag");
    }
    while (*p && *p != ',') p++;
    if (*p == ',') p++;

    
    txn_id_t handle = objid;
    obj_set_rw(&objid, (bool)writeflag);
    txn->objs[txn->num_objs++] = handle;
  }
}

/*
Parse a workload from a file and allocate buffer
*/
workload_t *parse_workload(const char *filename) {
  FILE *f = fopen(filename, "r");
  if (!f) FATAL("Failed to open transaction file");

  // count number of lines so we know how much to allocate
  int num_txns = count_lines(f);
  rewind(f);
  workload_t *workload = (workload_t*) malloc(sizeof(workload_t) + sizeof(txn_t) * num_txns);
  if (!workload) FATAL("Failed to malloc txn_list");
  workload->num_txns = num_txns;

  char buf[1000];
  int id = 0;
  while (fgets(buf, sizeof(buf), f)) {
    buf[strcspn(buf, "\n")] = 0;
    if (strlen(buf) > 0) {
      parse_txn(&workload->txns[id], id, buf);
      id++;
    }
  }

  return workload;
}
