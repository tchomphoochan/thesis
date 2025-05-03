#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pmutils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
Supported sizes
*/
#define MAX_CLIENTS 1
#define MAX_PUPPETS 16
#define SCHEDULER_CORE_ID 3
#define MAX_PENDING_PER_CLIENT 128
#define MAX_ACTIVE MAX_PUPPETS
#define MAX_SCHED_OUT 128

/*
Maximum number of objects per transaction
*/
#define PMHW_MAX_TXN_OBJS 16

/*
Object representation, supports up to 2^63 addresses
Use the top bit to identify whether it's a read or a write object
*/
typedef uint64_t obj_id_t;
static inline bool obj_is_write(obj_id_t id) {
  return (id & (1LLU << 63)) != 0;
}
static inline void obj_set_write(obj_id_t *id) {
  *id |= (1LLU << 63);
}
static inline void obj_set_read(obj_id_t *id) {
  *id &= ~(1LLU << 63);
}
static inline void obj_set_rw(obj_id_t *id, bool write) {
  *id = ((*id) & ~(1LLU << 63)) | (write ? (1LLU << 63) : 0);
}

/*
Puppetmaster transaction descriptor
*/
typedef uint64_t txn_id_t;
typedef uint64_t aux_data_t;
typedef struct {
  txn_id_t id;
  aux_data_t aux_data;
  size_t num_objs;
  obj_id_t objs[PMHW_MAX_TXN_OBJS];
} txn_t;

// Helper function
static inline bool check_txn_conflict(const txn_t *a, const txn_t *b) {
  for (size_t i = 0; i < a->num_objs; i++) {
    obj_id_t obj_a = a->objs[i] & ~(1ULL << 63);
    bool wr_a      = obj_is_write(a->objs[i]);

    for (size_t j = 0; j < b->num_objs; j++) {
      obj_id_t obj_b = b->objs[j] & ~(1ULL << 63);
      bool wr_b      = obj_is_write(b->objs[j]);

      if (obj_a == obj_b && (wr_a || wr_b))
        return true; // RW or WW conflict
    }
  }
  return false;
}
static inline void dump_txn(FILE *f, const txn_t *txn) {
  fprintf(f, "txn_t(id=%ld, aux_data=%ld, num_objs=%ld, reads={", txn->id, txn->aux_data, txn->num_objs);
  for (int i = 0; i < txn->num_objs; ++i) {
    obj_id_t obj = txn->objs[i] & ~(1ULL << 63);
    bool wr = obj_is_write(txn->objs[i]);
    if (!wr) fprintf(f, "%ld,", obj);
  }
  fprintf(f, "}, writes={");
  for (int i = 0; i < txn->num_objs; ++i) {
    obj_id_t obj = txn->objs[i] & ~(1ULL << 63);
    bool wr = obj_is_write(txn->objs[i]);
    if (wr) fprintf(f, "%ld,", obj);
  }
  fprintf(f, "}\n");
}


/*
Interfaces
*/

/*
Initialize Puppetmaster. Must be called before any other operations.
*/
void pmhw_init(int num_clients, int num_puppets);

/*
Clean up Puppetmaster.
*/
void pmhw_shutdown();

/*
Submit a new transaction descriptor to Puppetmaster.
*/
void pmhw_schedule(int client_id, const txn_t *txn);

/*
Poll for a scheduled transaction assigned to a puppet.
If a transaction becomes ready, fills in transactionId and puppetId.
May return PMHW_TIMEOUT if no transaction is ready within a timeout window.
Returns true if got a transaction, false if timed out.
*/
bool pmhw_poll_scheduled(txn_id_t *txn_id);

/*
Report that a previously assigned transaction has been completed by a puppet.
This signals the scheduler that the puppet is now idle and ready for new work.
*/
void pmhw_report_done(int worker_id, txn_id_t txn_id);

#ifdef __cplusplus
}
#endif

