from typing import *
from workload import Transaction, make_workload
import itertools
import copy

from abc import ABC

class Scheduler(ABC):
  def schedule(self: Self, txns: list[Transaction]) -> list[Transaction]:
    raise NotImplementedError

class GreedyScheduler(Scheduler):
  def schedule(_: Self, txns: list[Transaction]) -> list[Transaction]:
    main_txn = Transaction(set(), set(), set())
    sched_txns = []
    for txn in txns:
      if main_txn.compat(txn):
        main_txn = main_txn.merge(txn)
        sched_txns.append(txn)
    return sched_txns

class TournamentScheduler(Scheduler):
  def schedule(_: Self, all_txns: list[Transaction]) -> list[Transaction]:
    txns = copy.deepcopy(all_txns)
    while len(txns) > 1:
      new_txns = []
      for (ts1, ts2) in itertools.batched(txns, 2):
        new_txn = ts1.merge(ts2) if ts1.compat(ts2) else ts1
        new_txns.append(new_txn)
      txns = new_txns
    return [all_txns[id] for id in txns[0].ids]

if __name__ == "__main__":
  addr_space = list(range(2**15))
  workload = list(make_workload(addr_space, 128, 16, 0.0, 0.5))
  greedy = GreedyScheduler()
  print(len(greedy.schedule(workload)))
  tournament = TournamentScheduler()
  print(len(tournament.schedule(workload)))