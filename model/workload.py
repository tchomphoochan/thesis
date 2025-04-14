from typing import *
from dataclasses import dataclass
import itertools
import random

def make_zipf_weights(n: int, alpha: float):
  return [1.0/(i+1)**alpha for i in range(n)]

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
    return len(conflicts) == 0

  def merge(self: Self, other: Self) -> Self:
    assert self.compat(other), "Can only merge compatible transactions"
    return Transaction(
      ids = self.ids | other.ids,
      read_set = self.read_set | other.read_set,
      write_set = self.write_set | other.write_set
    )

def make_workload(addr_space: list[int], num_txn: int, num_elems_per_txn: int, zipf_param: float, write_probability: float):
  zipf = make_zipf_weights(len(addr_space), zipf_param)
  total_objs = num_txn * num_elems_per_txn

  all_objs = random.choices(population = addr_space, weights = zipf, k = total_objs)
  all_writes = random.choices(population = [False, True], weights = [1-write_probability, write_probability], k = total_objs)

  txn_objs = itertools.batched(all_objs, num_elems_per_txn)
  txn_writes = itertools.batched(all_writes, num_elems_per_txn)

  for id, objs, writes in zip(range(num_txn), txn_objs, txn_writes):
    write_set = frozenset({obj for obj, write in zip(objs, writes) if write})
    read_set = frozenset({obj for obj, write in zip(objs, writes) if not write})
    yield Transaction(ids=frozenset({id}), read_set=read_set, write_set=write_set)

if __name__ == "__main__":
  addr_space = list(range(2**10))
  print(list(make_workload(addr_space, 128, 16, 0.4, 0.5)))