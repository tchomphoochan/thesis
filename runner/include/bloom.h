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

// --- Bloom Filter Structure ---

typedef struct {
  uint64_t bits[BLOOM_TOTAL_BITS / 64];
} bloom_t;

// --- Internal Hashing Helper ---

static inline uint32_t bloom_hash(uint64_t x, int idx) {
  // Multiply-shift hashing, different constant per hash function
  static const uint64_t hash_constants[] = {
    0x9e3779b97f4a7c15ull, 0xc6a4a7935bd1e995ull,
    0x2545f4914f6cdd1dull, 0x21c64e4276c9f809ull
  };
  uint64_t h = x * hash_constants[idx];
  return (uint32_t)(h >> (64 - 12)); // 12 bits for 4096 entries
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

