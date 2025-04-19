"""
Playground for alternative hashing schemes like Cuckoo hashing
"""

import random
import itertools
import tqdm
import numpy as np
import pandas as pd

from typing import *
from bitarray import bitarray


K = TypeVar('K')
V = TypeVar('V')


class SimpleProbeHashTable[K,V]:
  hash_fn: Callable[[K, int], int]
  has: bitarray
  data: list[Tuple[K,V]|None]

  def __init__(self: Self, num_buckets: int, hash_fn: Callable[[K, int], int]):
    self.hash_fn = hash_fn
    self.has = bitarray(num_buckets)
    self.data = [None] * num_buckets

  def traverse(self, key: K) -> Tuple[int, bool]:
    """
    If key K exists, return the index containing that key and True.
    If key K does not exist, return the first empty slot this key could go into and False.
    """
    n = len(self.has)

    for attempt in range(n):
      idx = self.hash_fn(key, attempt)
      assert 0 <= idx < n

      if self.has[idx] and self.data[idx][0] == key:
        # Found the key
        return idx, True
      elif self.has[idx]:
        # Collision; keep probing
        pass
      else:
        # Found empty slot
        return idx, False

    raise MemoryError(f"Could not find empty slot for key {K!r}")

  def __setitem__(self: Self, key: K, value: V) -> None:
    idx, matched = self.traverse(key)
    self.has[idx] = True
    self.data[idx] = (key, value)

  def __getitem__(self: Self, key: K) -> V:
    idx, matched = self.traverse(key)
    if matched:
      return self.data[idx][1]
    else:
      raise KeyError(key)

  def items(self: Self):
    for has, kv in zip(self.has, self.data):
      if has:
        yield kv

  def keys(self: Self):
    return (key for key, _ in self.items())

  def values(self: Self):
    return (value for _, value in self.items())

  def __iter__(self: Self):
    return self.keys()

  def __repr__(self: Self):
    return type(self).__name__ + "(" + repr(self.data) +")"


class CuckooHashTable[K,V]:
  hash_fn: Callable[[K, int], int]
  num_tables: int
  has: bitarray
  data: list[Tuple[K,V]|None]
  insert_stats: list[int]

  def __init__(self: Self, num_buckets: int, num_tables: int, hash_fn: Callable[[K, int], int]):
    self.hash_fn = hash_fn
    self.num_tables = num_tables
    self.has = bitarray(num_buckets)
    self.data = [None] * num_buckets
    self.insert_stats = []

  def __getitem__(self: Self, key: K) -> V:
    # Check any of the num_tables candidates
    n = len(self.data)
    for attempt in range(self.num_tables):
      idx = self.hash_fn(key, attempt)
      assert 0 <= idx < n
      if self.has[idx] and self.data[idx][0] == key:
        return self.data[idx][1]
    raise KeyError(key)

  def __setitem__(self: Self, key: K, value: V):
    # Check any of the num_tables candidates
    n = len(self.data)
    for attempt in range(self.num_tables):
      idx = self.hash_fn(key, attempt)
      assert 0 <= idx < n
      if self.has[idx] and self.data[idx][0] == key:
        self.data[idx] = (key, value)
        self.has[idx] = True
        self.insert_stats.append(attempt+1)
        return
    
    # If not found, then start from the first table
    fst = False
    for attempt in range(n):
      idx = self.hash_fn(key, attempt % self.num_tables)
      assert 0 <= idx < n
      # If slot is empty, we're done
      if not self.has[idx]:
        self.data[idx] = (key, value)
        self.has[idx] = True
        self.insert_stats.append(n+attempt+1)
        return
      # Otherwise, swap the value with that slot
      # then keep trying
      (key, value), self.data[idx] = self.data[idx], (key, value)

    raise MemoryError("Infinite loop")

  def items(self: Self):
    for has, kv in zip(self.has, self.data):
      if has:
        yield kv

  def keys(self: Self):
    return (key for key, _ in self.items())

  def values(self: Self):
    return (value for _, value in self.items())

  def __repr__(self: Self):
    return type(self).__name__ + "(" + repr(self.data) +")"


N = 4*2**12
def my_hash(k, a):
  if a == 0:
    x = (k*98765431)
  elif a == 1:
    x = (k*536870911)
  elif a == 2:
    x = (k*1_000_000_007)
  elif a == 3:
    x = (k*1_000_000_009)
  else:
    raise Exception()

  x %= N
  return x

def fuzz_test(mine, ref):
  keys = set()
  for i in range(int(N//4 * 0.5)):
    k = random.randint(0, 2**30-1)
    while k in keys:
      k = random.randint(0, 2**30-1)
    keys.add(k)
  keys = list(keys)
  for i, k in tqdm.tqdm( list(enumerate(random.choices(keys, k=100000)) )):
    v = i
    mine[k] = v
    ref[k] = v
    assert dict(mine.items()) == dict(ref.items()), f"failed on iteration {i}"

if __name__ == "__main__":
  H = CuckooHashTable(N, 4, my_hash)
  fuzz_test(H, {})

  df = pd.DataFrame(H.insert_stats)
  print(df.describe())
