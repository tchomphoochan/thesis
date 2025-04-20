from typing import *
from dataclasses import dataclass
from bloom_filter import Set, make_parallel_bloom_filter_family
import itertools
import random
import numpy as np
import matplotlib.pyplot as plt

def make_zipf_weights(n: int, alpha: float) -> np.array:
  return 1.0/(np.arange(n)+1)**alpha

@dataclass(frozen=True)
class Transaction:
  ids: frozenset[int]
  read_set: frozenset[int]
  write_set: frozenset[int]

  def compat(ts1: Self, ts2: Self) -> bool:
    r1w2_set = ts1.read_set & ts2.write_set
    w1r2_set = ts1.write_set & ts2.read_set
    w1w2_set = ts1.write_set & ts2.write_set
    conflicts = r1w2_set | w1r2_set | w1w2_set
    return not bool(conflicts)

  def merge(ts1: Self, ts2: Self) -> Self:
    assert ts1.compat(ts2), "Can only merge compatible transactions"
    return Transaction(
      ids = ts1.ids | ts2.ids,
      read_set = ts1.read_set | ts2.read_set,
      write_set = ts1.write_set | ts2.write_set
    )

def make_workload(addr_space: np.array, num_txn: int, num_elems_per_txn: int, zipf_param: float, write_probability: float) -> list[Transaction]:
  zipf = make_zipf_weights(len(addr_space), zipf_param)
  total_objs = num_txn * num_elems_per_txn

  all_objs = random.choices(population = addr_space, weights = zipf, k = total_objs)
  all_writes = random.choices(population = [False, True], weights = [1-write_probability, write_probability], k = total_objs)

  txn_objs = itertools.batched(all_objs, num_elems_per_txn)
  txn_writes = itertools.batched(all_writes, num_elems_per_txn)

  txns = []
  for id, objs, writes in zip(range(num_txn), txn_objs, txn_writes):
    write_set = frozenset({obj for obj, write in zip(objs, writes) if write})
    read_set = frozenset({obj for obj, write in zip(objs, writes) if not write})
    txns.append(Transaction(ids=frozenset({id}), read_set=read_set, write_set=write_set))
  return txns

def compress_transaction(transaction: Transaction, family: Callable[[], Set]) -> Transaction:
  """
  Compress transaction from exact set representation into bloom filter representation
  """
  read_set = family()
  for obj in transaction.read_set:
    read_set.add(obj)
  write_set = family()
  for obj in transaction.write_set:
    write_set.add(obj)
  new_txn = Transaction(ids=transaction.ids, read_set=read_set, write_set=write_set)
  return new_txn

def compress_workload(workload: list[Transaction], family: Callable[[], Set]) -> list[Transaction]:
  return [compress_transaction(txn, family) for txn in workload]

if __name__ == "__main__":
  addr_space = list(range(2**10))
  workload = make_workload(addr_space, 128, 16, 0.0, 0.05)
  family = make_parallel_bloom_filter_family(1024, 4)
