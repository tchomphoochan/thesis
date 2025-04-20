#!/usr/bin/env python3

import numpy as np
import matplotlib.pyplot as plt
import random
import time
import sys
import itertools

from concurrent.futures import ProcessPoolExecutor
from typing import *

from bloom_filter import Set, make_bloom_filter, make_parallel_bloom_filter

def get_false_pos_rate(addr_space: list[int], num_elems: int, len_signature: int, num_partitions: int):
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
  max_log = 18
  len_addr_space = 2**max_log
  num_elems_arr = np.rint(2**np.arange(0, 12, 1.0/4.0)).astype(int)
  addr_space = list(range(len_addr_space))
  random.shuffle(addr_space)

  with ProcessPoolExecutor() as exec:
    rates = list(exec.map(get_false_pos_rate,
                          itertools.repeat(addr_space),
                          num_elems_arr,
                          itertools.repeat(len_signature),
                          itertools.repeat(num_partitions)))

  rates = np.array(rates)
  return (num_elems_arr, rates, f'm={len_signature}, k={num_partitions}')

def graph_false_pos_rate():
  config_arrs = [
    # same m/k
    [(128, 1), (256, 2), (512, 4), (1024, 8)],
    # same k, increasing m
    [(128, 4), (256, 4), (512, 4), (1024, 4)],
    # same m, increasing k
    [(1024, 1), (1024, 2), (1024, 4), (1024, 8)],
    # common configs
    [(512, 4), (512, 8), (1024, 4), (1024, 8)],
  ]

  for index, config_arr in enumerate(config_arrs):
    begin = time.time()

    plt.figure()
    plt.title("False positive rate of parallel bloom filters")
    plt.xlabel("Number of elements inserted ($n$)")
    plt.ylabel("False positive rate")
    plt.xscale('log', base=2)
    plt.yscale('linear')
    plt.grid()

    for x, y, label in map(get_data_for_config, *zip(*config_arr)):
      plt.plot(x, y, '-', label=label)

    plt.legend()
    end = time.time()

    filename = f"output-bloom-filter-{index}.svg"
    print(f"Took {end-begin} seconds to render {filename}", file=sys.stderr)
    plt.savefig(filename)

if __name__ == "__main__":
  graph_false_pos_rate()
