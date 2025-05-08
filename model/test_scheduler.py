#!/usr/bin/env python3

import time
import itertools
import sys
import numpy as np
import matplotlib.pyplot as plt
import functools

from typing import *
from concurrent.futures import ProcessPoolExecutor

from workload import Transaction, make_workload
from scheduler import Scheduler, GreedyScheduler, TournamentScheduler

SchedType = Literal["greedy", "tournament"]
SCHED_TYPES = ["greedy", "tournament"]

NUM_OBJS_PER_TXN = 16
NUM_TXNS = 2**7

def _get_num_txns_scheduled(mem_size: int, zipf_param: float, write_prob: float, sched_type: SchedType):
  addr_space = np.arange(mem_size)
  workload = list(make_workload(addr_space, NUM_TXNS, NUM_OBJS_PER_TXN, zipf_param, write_prob))

  s: Scheduler
  if sched_type == "greedy":
    s = GreedyScheduler()
  elif sched_type == "tournament":
    s = TournamentScheduler()

  return len(s.schedule(workload))

def get_num_txns_scheduled(mem_size: float, zipf_param: float, write_prob: float, sched_type: SchedType):
  num_trials = 21
  with ProcessPoolExecutor() as exec:
    y = list(exec.map(_get_num_txns_scheduled,
                      [mem_size] * num_trials,
                      [zipf_param] * num_trials,
                      [write_prob] * num_trials,
                      [sched_type] * num_trials))
  return np.median(y, axis=0)

def graph_scale_num_objs():

  for workload_type, omega in [('Read-heavy', 0.05), ('Write-heavy', 0.50)]:
    filename = f"output-scheduler-{workload_type.lower()}-{NUM_TXNS}x{NUM_OBJS_PER_TXN}.svg"
    print(f"Rendering: {filename}", file=sys.stderr)
    begin = time.time()
    plt.figure(figsize=(6.4, 4), dpi=200)
    plt.title(f"{workload_type} workload: {NUM_OBJS_PER_TXN} objs/txn, {omega*100:.0f}% probability of writing")
    plt.xlabel("Number of records in the database")
    plt.ylabel(f"Number of transactions scheduled (max possible: {NUM_TXNS})")
    plt.xscale("log", base=2)
    plt.yscale("linear")
    plt.grid()

    for sched_type, line in zip(SCHED_TYPES, ["--", "-"]):
      for theta in [0.0, 0.6, 0.8]:
        x = 2**np.arange(10,25,1)
        y = np.array(list(map(get_num_txns_scheduled,
                              x,
                              itertools.repeat(theta),
                              itertools.repeat(omega),
                              itertools.repeat(sched_type))))
        plt.plot(x, y, line, label=f"$\\theta = {theta}$, {sched_type}")

    plt.legend()
    end = time.time()
    print(f"Done: {filename} {end-begin} seconds", file=sys.stderr)
    plt.savefig(filename)

if __name__ == "__main__":
  graph_scale_num_objs()
