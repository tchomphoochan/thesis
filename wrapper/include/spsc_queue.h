#pragma once

#include <stdatomic.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// ========== Helper Macro ==========
#define _SPSC_CAT(a, b) a##b
#define SPSC_CAT(a, b) _SPSC_CAT(a, b)

// ========== Core Template ==========
#define SPSC_QUEUE_IMPL(TYPE, NAME) \
typedef struct { \
    alignas(64) atomic_int head; char _pad1[64-sizeof(atomic_int)]; \
    alignas(64) atomic_int tail; char _pad2[64-sizeof(atomic_int)]; \
    TYPE *buffer; int capacity; int cached_head; int cached_tail; \
    char _pad[64 - sizeof(TYPE*) - sizeof(int)*3]; \
} NAME; \
                                                                                 \
static inline void SPSC_CAT(spsc_queue_init_, NAME)(NAME *q, int capacity) {     \
  q->buffer = malloc(sizeof(TYPE) * capacity);                                   \
  q->capacity = capacity;                                                        \
  atomic_init(&q->head, 0);                                                      \
  atomic_init(&q->tail, 0);                                                      \
  q->cached_head = 0;                                                            \
  q->cached_tail = 0;                                                            \
}                                                                                \
                                                                                 \
static inline void SPSC_CAT(spsc_queue_free_, NAME)(NAME *q) {                   \
  free(q->buffer);                                                               \
  q->buffer = NULL;                                                              \
  q->capacity = 0;                                                               \
}                                                                                \
                                                                                 \
static inline bool SPSC_CAT(spsc_enqueue_, NAME)(NAME *q, const TYPE *item) {           \
  int tail = atomic_load_explicit(&q->tail, memory_order_relaxed);              \
  int next_tail = (tail + 1) % q->capacity;                                      \
  if (next_tail == q->cached_head) {                                             \
    q->cached_head = atomic_load_explicit(&q->head, memory_order_acquire);      \
    if (next_tail == q->cached_head)                                            \
      return false; /* full */                                                  \
  }                                                                              \
  q->buffer[tail] = *item;                                                        \
  atomic_store_explicit(&q->tail, next_tail, memory_order_release);             \
  return true;                                                                   \
}                                                                                \
                                                                                 \
static inline bool SPSC_CAT(spsc_peek_, NAME)(NAME *q, TYPE *item) {          \
  int head = atomic_load_explicit(&q->head, memory_order_relaxed);              \
  if (head == q->cached_tail) {                                                  \
    q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);      \
    if (head == q->cached_tail)                                                  \
      return false; /* empty */                                                  \
  }                                                                              \
  *item = q->buffer[head];                                                       \
  atomic_store_explicit(&q->head, head, memory_order_release); \
  return true;                                                                   \
}                                                                                \
                                                                                 \
static inline bool SPSC_CAT(spsc_dequeue_, NAME)(NAME *q, TYPE *item) {          \
  int head = atomic_load_explicit(&q->head, memory_order_relaxed);              \
  if (head == q->cached_tail) {                                                  \
    q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);      \
    if (head == q->cached_tail)                                                  \
      return false; /* empty */                                                  \
  }                                                                              \
  *item = q->buffer[head];                                                       \
  atomic_store_explicit(&q->head, (head + 1) % q->capacity, memory_order_release); \
  return true;                                                                   \
}                                                                                
