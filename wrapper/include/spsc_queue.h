#pragma once

#include <stdatomic.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pmutils.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Helper Macro ==========
#define _SPSC_CAT(a, b) a##b
#define SPSC_CAT(a, b) _SPSC_CAT(a, b)

// ========== Core Template ==========
#define SPSC_NO_LOG 
#define SPSC_QUEUE_IMPL(DATATYPE, PREFIX, TYPENAME) \
typedef struct { \
    alignas(64) atomic_int head; char _pad1[64-sizeof(atomic_int)]; \
    alignas(64) atomic_int tail; char _pad2[64-sizeof(atomic_int)]; \
    DATATYPE *buffer; int capacity; int mask; \
    char _pad[64 - sizeof(DATATYPE*) - sizeof(int)*2]; \
} TYPENAME; \
\
static inline void SPSC_CAT(PREFIX, _init)(TYPENAME *q, int capacity) { \
  ASSERT((capacity & (capacity-1)) == 0); \
  q->buffer = malloc(sizeof(DATATYPE) * capacity); \
  q->capacity = capacity; \
  q->mask = capacity-1; \
  atomic_init(&q->head, 0); \
  atomic_init(&q->tail, 0); \
} \
\
static inline void SPSC_CAT(PREFIX, _free)(TYPENAME *q) { \
  free(q->buffer); \
  q->buffer = NULL; \
  q->capacity = 0; \
} \
\
static inline bool SPSC_CAT(PREFIX, _enq)(TYPENAME *q, const DATATYPE *item) { \
  int tail = atomic_load_explicit(&q->tail, memory_order_relaxed); \
  int next_tail = (tail + 1) & q->mask; \
  int head = atomic_load_explicit(&q->head, memory_order_acquire); \
  if (next_tail == head) { \
      return false; /* full */ \
  } \
  q->buffer[tail] = *item; \
  atomic_store_explicit(&q->tail, next_tail, memory_order_release); \
  return true; \
} \
\
static inline bool SPSC_CAT(PREFIX, _full)(TYPENAME *q) { \
  int tail = atomic_load_explicit(&q->tail, memory_order_relaxed); \
  int next_tail = (tail + 1) & q->mask; \
  int head = atomic_load_explicit(&q->head, memory_order_acquire); \
  if (next_tail == head) { \
      return true; /* full */ \
  } \
  return false; \
} \
\
static inline bool SPSC_CAT(PREFIX, _peek)(TYPENAME *q, DATATYPE *item) { \
  int head = atomic_load_explicit(&q->head, memory_order_relaxed); \
  int tail = atomic_load_explicit(&q->tail, memory_order_acquire); \
  if (head == tail) { \
      return false; /* empty */ \
  } \
  *item = q->buffer[head]; \
  return true; \
} \
\
static inline bool SPSC_CAT(PREFIX, _deq)(TYPENAME *q, DATATYPE *item) { \
  int head = atomic_load_explicit(&q->head, memory_order_relaxed); \
  int tail = atomic_load_explicit(&q->tail, memory_order_acquire); \
  if (head == tail) { \
      return false; /* empty */ \
  } \
  *item = q->buffer[head]; \
  atomic_store_explicit(&q->head, (head + 1) & q->mask, memory_order_release); \
  return true; \
}

#ifdef __cplusplus
}
#endif
