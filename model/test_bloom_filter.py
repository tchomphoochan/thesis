#!/usr/bin/env python3

import numpy as np
import random
import time
import sys
import itertools
import tqdm

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

from concurrent.futures import Executor, Future, ProcessPoolExecutor
from typing import *

from bloom_filter import Set, make_bloom_filter, make_parallel_bloom_filter

def get_false_pos_rate(addr_space: list[int], num_elems: int, len_signature: int, num_partitions: int):
  random.seed(1357924680)
  ft: Set = make_parallel_bloom_filter(len_signature, num_partitions)

  for i in range(num_elems):
    elem = addr_space[i]
    ft.add(elem)

  num_false_pos = 0
  num_samples = len(addr_space) - num_elems
  for i in range(num_elems, len(addr_space)):
    elem = addr_space[i]
    if elem in ft:
      num_false_pos += 1
  false_pos_rate = num_false_pos / num_samples

  return false_pos_rate

def get_data_for_config(len_signature: int, num_partitions: int) -> Tuple[np.array, np.array, str]:
  print(f"  m={len_signature}, k={num_partitions}", file=sys.stderr)
  max_log = 20
  len_addr_space = 2**max_log
  num_elems_arr = np.rint(2**np.arange(0, 14, 1.0/4.0)).astype(int)
  addr_space = list(range(len_addr_space))
  random.shuffle(addr_space)

  with tqdm.tqdm(total=len(num_elems_arr)) as progress:
    with ProcessPoolExecutor() as pool:
      futures = []
      for num_elems in num_elems_arr:
        future = pool.submit(get_false_pos_rate, addr_space, num_elems, len_signature, num_partitions)
        future.add_done_callback(lambda _: progress.update())
        futures.append(future)
      rates = [future.result() for future in futures]

  rates = np.array(rates)
  return (num_elems_arr, rates, f'm={len_signature}, k={num_partitions}')

def graph_false_pos_rate():
  plt.rcParams['axes.formatter.min_exponent'] = 20

  config_arrs = [
    # same m/k (number of bits overall increase)
    [(512, 1), (1024, 2), (2048, 4), (4096, 8), (8192, 16)],
    # same k, increasing m
    [(512, 4), (1024, 4), (2048, 4), (4096, 4), (8192, 4)],
    # same m, increasing k
    [(1024, 1), (1024, 2), (1024, 4), (1024, 8)],
    # same as above
    [(4096, 1), (4096, 2), (4096, 4), (4096, 8)],
    None,
  ]

  for index, config_arr in enumerate(config_arrs):
    filename = f"output-bloom-filter-{index}.svg"
    action = "Rendering" if config_arr else "Skipping"
    print(f"{action}: {filename}", file=sys.stderr)
    if not config_arr:
      continue

    begin = time.time()

    fig, ax = plt.subplots(figsize=(6.4, 4), dpi=200)
    ax.set_title("False positive rate of parallel bloom filters")

    ax.set_xlabel("Number of elements inserted ($n$)")
    ax.set_ylabel("False positive rate")
    ax.set_xscale('log', base=2)
    ax.set_yscale('log', base=10)
    ax.set_ylim(1e-4-3e-5, 1+0.3)
    ax.grid()

    # for axis in [ax.xaxis, ax.yaxis]:
    #   formatter = FuncFormatter(lambda y, _: '{:.8g}'.format(y))
    #  axis.set_major_formatter(formatter)

    for x, y, label in map(get_data_for_config, *zip(*config_arr)):
      ax.plot(x, y, '-', label=label)

    ax.legend()
    end = time.time()

    print(f"Done: {filename}, {end-begin}", file=sys.stderr)
    fig.savefig(filename)

if __name__ == "__main__":
  graph_false_pos_rate()
