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
#include "st_queue.h"

SPSC_QUEUE_IMPL(txn_id_t, spsc_tid, spsc_tid_t)
SPSC_QUEUE_IMPL(txn_t, spsc_txn, spsc_txn_t)
ST_QUEUE_IMPL(txn_t, stq_txn, stq_txn_t)

static spsc_txn_t pending_qs[MAX_CLIENTS];
static spsc_tid_t sched_qs[MAX_PUPPETS];
static spsc_tid_t done_qs[MAX_PUPPETS];

static int num_clients = 0;
static int num_puppets = 0;
static stq_txn_t active_txns[MAX_PUPPETS];

static pthread_t scheduler_thread;
static atomic_bool scheduler_running = ATOMIC_VAR_INIT(false);

static bool conflicts_with_active(const txn_t *new_txn) {
  for (int puppet = 0; puppet < num_puppets; ++puppet) {
    int j = active_txns[puppet].head;
    while (j != active_txns[puppet].tail) {
      if (check_txn_conflict(new_txn, &active_txns[puppet].buffer[j])) {
        return true;
      }
      j = (j+1) & active_txns[puppet].mask;
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

  int current_puppet_id = 0;

  while (atomic_load_explicit(&scheduler_running, memory_order_relaxed)) {

    // Drain done queue
    for (int puppet = 0; puppet < num_puppets; ++puppet) {
      if (stq_txn_empty(&active_txns[puppet])) {
        DEBUG("skipping puppet %d done queue because no active txns", puppet);
        continue;
      }
      txn_id_t txn_id;
      while (spsc_tid_deq(&done_qs[puppet], &txn_id)) {
        DEBUG("done queue of puppet %d has tid %d", puppet, txn_id);
        // find the transaction in active list
        // we expect the worker to return transaction in FIFO order
        txn_t txn;
        ASSERT(stq_txn_deq(&active_txns[puppet], &txn));
        ASSERT(txn.id == txn_id);
        pmlog_record(txn_id, PMLOG_CLEANUP, -1LLU);
      }
    }

    // Drain pending queue
    for (int client = 0; client < num_clients; ++client) {
      // No space to schedule, break
      if (stq_txn_full(&active_txns[current_puppet_id])) {
        DEBUG("active_txn for current puppet %d is full, so no more scheduling", current_puppet_id);
        break;
      }

      txn_t txn;
      DEBUG("now peeking transaction in pending queue");
      while (spsc_txn_peek(&pending_qs[client], &txn)) {
        DEBUG("found a transaction id %d", txn.id);
        // If conflict, also break
        if (conflicts_with_active(&txn)) {
          DEBUG("it conflicts");
          break;
        }

        // If successfully scheduled, then must put it in our active list
        ASSERT(spsc_txn_deq(&pending_qs[client], &txn));
        ASSERT(stq_txn_enq(&active_txns[current_puppet_id], txn));
        DEBUG("removed from pending, enqueued to active");

        // Log and send message to the user
        pmlog_record(txn.id, PMLOG_SCHED_READY, current_puppet_id);
        DEBUG("enqueing to scheuled queue of %d", current_puppet_id);
        ASSERT(spsc_tid_enq(&sched_qs[current_puppet_id], &txn.id));

        // Move to next puppet in round robin oder
        current_puppet_id = (current_puppet_id + 1) % num_puppets;
        DEBUG("now moving onto %d", current_puppet_id);

        // No space to schedule more, break
        if (stq_txn_full(&active_txns[current_puppet_id])) {
          DEBUG("puppet %d is full", current_puppet_id);
          break;
        }
      }
    }
  }
  return NULL;
}

// === Interface Implementations ===

void pmhw_init(int num_clients_, int num_puppets_) {
  ASSERT(!atomic_load_explicit(&scheduler_running, memory_order_acquire));
  num_clients = num_clients_;
  num_puppets = num_puppets_;
  ASSERT(num_clients <= MAX_CLIENTS);
  ASSERT(num_puppets <= MAX_PUPPETS);

  // Internal bujffer
  for (int i = 0; i < MAX_PUPPETS; ++i) stq_txn_init(&active_txns[i], MAX_ACTIVE_PER_PUPPET);

  // Initialize all the queues
  for (int i = 0; i < MAX_CLIENTS; ++i) spsc_txn_init(&pending_qs[i], MAX_PENDING_PER_CLIENT);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_init(&done_qs[i], MAX_ACTIVE_PER_PUPPET);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_init(&sched_qs[i], MAX_ACTIVE_PER_PUPPET);

  // Mark the scheduler running
  atomic_store_explicit(&scheduler_running, true, memory_order_release);
  
  // Start the loop
  EXPECT_OK(pthread_create(&scheduler_thread, NULL, scheduler_loop, NULL) == 0);
}

void pmhw_shutdown() {
  ASSERT(atomic_load_explicit(&scheduler_running, memory_order_acquire));
  atomic_store_explicit(&scheduler_running, false, memory_order_release);

  EXPECT_OK(pthread_join(scheduler_thread, NULL) == 0);
  for (int i = 0; i < MAX_PUPPETS; ++i) stq_txn_free(&active_txns[i]);
  for (int i = 0; i < MAX_CLIENTS; ++i) spsc_txn_free(&pending_qs[i]);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_free(&done_qs[i]);
  for (int i = 0; i < MAX_PUPPETS; ++i) spsc_tid_free(&sched_qs[i]);
}

void pmhw_schedule(int client_id, const txn_t *txn) {
  ASSERT(txn);
  pmlog_record(txn->id, PMLOG_SUBMIT, -1LLU);
  while (!spsc_txn_enq(&pending_qs[client_id], txn));
}

bool pmhw_poll_scheduled(int puppet_id, txn_id_t *txn_id) {
  ASSERT(txn_id);
  uint32_t cnt = 0;
  while ((cnt++ & (1<<20)) || atomic_load_explicit(&scheduler_running, memory_order_relaxed)) {
    if (spsc_tid_deq(&sched_qs[puppet_id], txn_id)) return true;
  }
  return false;
}

void pmhw_report_done(int puppet_id, txn_id_t txn_id) {
  pmlog_record(txn_id, PMLOG_DONE, puppet_id);
  while (!spsc_tid_enq(&done_qs[puppet_id], &txn_id));
}

