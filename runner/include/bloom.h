// bloom.h - Bloom filter interface for conflict checking
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Configurable Parameters ---

// Total number of bits in the Bloom filter
#ifndef BLOOM_TOTAL_BITS
#define BLOOM_TOTAL_BITS 65536
#endif

// Number of hash functions
#ifndef BLOOM_NUM_HASHES
#define BLOOM_NUM_HASHES 4
#endif

// Conditions on parameters
#if BLOOM_TOTAL_BITS % BLOOM_NUM_HASHES != 0
#error "BLOOM_NUM_HASHES must evently divide BLOOM_TOTAL_BITS"
#endif
#if (BLOOM_TOTAL_BITS / BLOOM_NUM_HASHES) % 64 != 0
#error "Each partition of Bloom filter must have number of bits divisible by 64"
#endif

// --- Bloom Filter Structure ---

typedef struct {
  uint64_t bits[BLOOM_TOTAL_BITS / 64];
} bloom_t;

// --- Internal Hashing Helper ---

static inline uint32_t bloom_hash(uint64_t x, int idx) {
  // Multiply-shift hashing, different constant per hash function
  static const uint64_t hash_constants[] = {
    0x9e3779b97f4a7c15ull, 0xc6a4a7935bd1e995ull,
    0x2545f4914f6cdd1dull, 0x21c64e4276c9f809ull,
    0x5851f42d4c957f2dull, 0xda942042e4dd58b5ull,
    0x14057b7ef767814full, 0x2f8b15c6c8b3a3c5ull
  };
  uint64_t h = x * hash_constants[idx];
  return (h >> 46) % (BLOOM_TOTAL_BITS / BLOOM_NUM_HASHES);
}

// --- API ---

// Initialize (zero) the Bloom filter
static inline void bloom_init(bloom_t *bf) {
  memset(bf, 0, sizeof(*bf));
}

// Insert an object ID into the Bloom filter
static inline void bloom_insert(bloom_t *bf, uint64_t objid) {
  for (int i = 0; i < BLOOM_NUM_HASHES; ++i) {
    uint32_t bitpos = bloom_hash(objid, i);
    bf->bits[bitpos / 64] |= (1ull << (bitpos % 64));
  }
}

// Query whether an object ID *may* be present (could be false positive)
static inline bool bloom_query(const bloom_t *bf, uint64_t objid) {
  for (int i = 0; i < BLOOM_NUM_HASHES; ++i) {
    uint32_t bitpos = bloom_hash(objid, i);
    if (!(bf->bits[bitpos / 64] & (1ull << (bitpos % 64)))) {
      return false;
    }
  }
  return true;
}

#ifdef __cplusplus
}
#endif

