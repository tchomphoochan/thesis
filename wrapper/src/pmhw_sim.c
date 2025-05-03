#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>
#include <sys/sysinfo.h>

#include "pmhw.h"
#include "pmlog.h"
#include "spsc_queue.h"

SPSC_QUEUE_IMPL(txn_id_t, spsc_tid, spsc_tid_t)
SPSC_QUEUE_IMPL(txn_t, spsc_txn, spsc_txn_t)

static spsc_txn_t pending_qs[MAX_CLIENTS];
static spsc_tid_t sched_q;
static spsc_tid_t done_qs[MAX_PUPPETS];

static int num_clients = 0;
static int num_puppets = 0;
static int num_active_txns = 0;
static txn_t active_txns[MAX_ACTIVE];

static pthread_t scheduler_thread;
static volatile bool scheduler_running = false;

static bool has_conflict(const txn_t *a, const txn_t *b) {
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

static bool conflicts_with_active(const txn_t *new_txn) {
  for (int i = 0; i < num_active_txns; ++i) {
    if (has_conflict(new_txn, &active_txns[i])) {
      return true;
    }
  }
  return false;
}

/*
The scheduler thread
*/
static void *scheduler_loop(void *arg) {
  pin_thread_to_core(SCHEDULER_CORE_ID);
  (void)arg;

  while (scheduler_running) {

    // Drain done queue
    for (int puppet = 0; puppet < num_puppets; ++puppet) {
      txn_id_t txn_id;
      if (spsc_tid_deq(&done_qs[puppet], &txn_id)) {
        // find the transaction in active list
        int i;
        bool found = false;
        for (i = 0; i < num_active_txns; ++i) {
          if (active_txns[i].id == txn_id) {
            found = true;
            break;
          }
        }
        ASSERT(found);

        // remove transaction txn_id from active
        active_txns[i] = active_txns[--num_active_txns];
        pmlog_record(txn_id, PMLOG_CLEANUP, -1LLU);
      }
    }

    // Drain pending queue
    for (int client = 0; client < num_clients; ++client) {
      txn_t txn;
      while (spsc_txn_peek(&pending_qs[client], &txn)) {
        // No space to schedule, break
        if (num_active_txns >= MAX_ACTIVE) {
          break;
        }
        // If conflict, also break
        if (conflicts_with_active(&txn)) {
          break;
        }

        // Found a good transaction, try to schedule it
        bool success = spsc_tid_enq(&sched_q, txn.id);
        if (!success) { break; }

        // If successfully scheduled, then must put it in our active list
        ASSERT(num_active_txns < MAX_ACTIVE);
        active_txns[num_active_txns++] = txn;

        // Log and dequeue
        pmlog_record(txn.id, PMLOG_SCHED_READY, 0);
        ASSERT(spsc_txn_deq(&pending_qs[client], &txn));
      }

      // No space to schedule, break
      if (num_active_txns >= MAX_ACTIVE) {
        break;
      }
    }
  }
  return NULL;
}

// === Interface Implementations ===

void pmhw_init(int num_clients_, int num_puppets_) {
  ASSERT(!scheduler_running);
  num_clients = num_clients_;
  num_puppets = num_puppets_;
  ASSERT(num_clients <= MAX_CLIENTS);
  ASSERT(num_puppets <= MAX_PUPPETS);

  // Initialize all the queues
  for (int i = 0; i < MAX_CLIENTS; ++i) spsc_txn_init(&pending_qs[i], MAX_PENDING_PER_CLIENT);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_init(&done_qs[i], MAX_ACTIVE);
  spsc_tid_init(&sched_q, MAX_SCHED_OUT);

  // Mark the scheduler running
  scheduler_running = true;
  
  // Start the loop
  EXPECT_OK(pthread_create(&scheduler_thread, NULL, scheduler_loop, NULL) == 0);
}

void pmhw_shutdown() {
  ASSERT(scheduler_running);

  scheduler_running = false;
  EXPECT_OK(pthread_join(scheduler_thread, NULL) == 0);

  for (int i = 0; i < MAX_CLIENTS; ++i) spsc_txn_free(&pending_qs[i]);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_free(&done_qs[i]);
  spsc_tid_free(&sched_q);
}

bool pmhw_schedule(int client_id, const txn_t *txn) {
  ASSERT(scheduler_running);
  ASSERT(txn);
  // must log before actually adding to the queue
  // otherwise PMLOG_INPUT_RECV might actually be too fast
  return spsc_txn_enq(&pending_qs[client_id], *txn);
}

bool pmhw_poll_scheduled(txn_id_t *txn_id) {
  ASSERT(txn_id);
  if (!scheduler_running) return false;
  return spsc_tid_deq(&sched_q, txn_id);
}

void pmhw_report_done(int puppet_id, txn_id_t txn_id) {
  ASSERT(scheduler_running);
  ASSERT(spsc_tid_enq(&done_qs[puppet_id], txn_id));
}

