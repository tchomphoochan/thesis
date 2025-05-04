#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pmutils.h"

#ifdef __cplusplus
extern "C" {
#endif

// ========== Helper Macro ==========
#define _STQ_CAT(a, b) a##b
#define STQ_CAT(a, b) _STQ_CAT(a, b)

// ========== Core Template ==========
#define ST_QUEUE_IMPL(DATATYPE, PREFIX, TYPENAME) \
typedef struct { \
  int head; \
  int tail; \
  int capacity; \
  int mask; \
  DATATYPE *buffer; \
  char _pad[64 - sizeof(int)*4 - sizeof(DATATYPE*)]; \
} TYPENAME; \
\
static inline void STQ_CAT(PREFIX, _init)(TYPENAME *q, int capacity) { \
  ASSERT((capacity & (capacity-1)) == 0); \
  q->head = 0; \
  q->tail = 0; \
  q->capacity = capacity; \
  q->mask = capacity-1; \
  q->buffer = (DATATYPE *) malloc(sizeof(DATATYPE) * capacity); \
} \
\
static inline void STQ_CAT(PREFIX, _free)(TYPENAME *q) { \
  free(q->buffer); \
  q->buffer = NULL; \
  q->capacity = 0; \
  q->head = 0; \
  q->tail = 0; \
} \
\
static inline bool STQ_CAT(PREFIX, _enq)(TYPENAME *q, DATATYPE item) { \
  int next_tail = (q->tail + 1) & q->mask; \
  if (next_tail == q->head) return false; \
  q->buffer[q->tail] = item; \
  q->tail = next_tail; \
  return true; \
} \
\
static inline bool STQ_CAT(PREFIX, _deq)(TYPENAME *q, DATATYPE *item) { \
  if (q->head == q->tail) return false; \
  *item = q->buffer[q->head]; \
  q->head = (q->head + 1) & q->mask; \
  return true; \
} \
\
static inline bool STQ_CAT(PREFIX, _peek)(TYPENAME *q, DATATYPE *item) { \
  if (q->head == q->tail) return false; \
  *item = q->buffer[q->head]; \
  return true; \
} \
\
static inline bool STQ_CAT(PREFIX, _full)(TYPENAME *q) { \
  return ((q->tail + 1) & q->mask) == q->head; \
} \
\
static inline bool STQ_CAT(PREFIX, _empty)(TYPENAME *q) { \
  return q->head == q->tail; \
} \
\
static inline int STQ_CAT(PREFIX, _length)(TYPENAME *q) { \
  return (q->tail - q->head + q->capacity) & q->mask; \
}

#ifdef __cplusplus
}
#endif
