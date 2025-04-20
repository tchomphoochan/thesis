from typing import *
from workload import Transaction, make_workload, compress_workload
from bloom_filter import Set, make_parallel_bloom_filter_family
import itertools
import copy

from abc import ABC

class Scheduler(ABC):
  def schedule(self: Self, txns: list[Transaction]) -> list[Transaction]:
    raise NotImplementedError

class GreedyScheduler(Scheduler):
  def schedule(_: Self, txns: list[Transaction]) -> list[Transaction]:
    main_txn = txns[0]
    sched_txns = [txns[0]]
    for txn in txns[1:]:
      if main_txn.compat(txn):
        main_txn = main_txn.merge(txn)
        sched_txns.append(txn)
    return sched_txns

class TournamentScheduler(Scheduler):
  def schedule(_: Self, all_txns: list[Transaction]) -> list[Transaction]:
    txns = copy.copy(all_txns)
    while len(txns) > 1:
      assert len(txns) % 2 == 0
      new_txns = []
      for (ts1, ts2) in itertools.batched(txns, 2):
        new_txn = ts1.merge(ts2) if ts1.compat(ts2) else ts1
        new_txns.append(new_txn)
      txns = new_txns
    return [all_txns[id] for id in txns[0].ids]

class CompressedScheduler(Scheduler):
  underlying: Scheduler
  family: Callable[[], Set]

  def __init__(self, underlying: Scheduler, family: Callable[[], Set]):
    self.underlying = underlying
    self.family = family

  def schedule(self: Self, all_txns: list[Transaction]) -> list[Transaction]:
    txns = compress_workload(all_txns, self.family)
    return self.underlying.schedule(txns)

if __name__ == "__main__":
  addr_space = list(range(2**24))
  workload = list(make_workload(addr_space, 256, 16, 0.0, 0.5))
  family = make_parallel_bloom_filter_family(1024, 4)

  greedy = GreedyScheduler()
  print(len(greedy.schedule(workload)))

  greedy_c = CompressedScheduler(greedy, family)
  print(len(greedy_c.schedule(workload)))

  tournament = TournamentScheduler()
  print(len(tournament.schedule(workload)))

  tournament_c = CompressedScheduler(tournament, family)
  print(len(tournament_c.schedule(workload)))
