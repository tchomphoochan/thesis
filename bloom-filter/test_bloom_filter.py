#!/usr/bin/env python3

from bloom_filter import Set, make_bloom_filter, make_parallel_bloom_filter
import numpy as np
import matplotlib.pyplot as plt
import random

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

def graph_false_pos_rate():
  max_log = 18
  len_addr_space = 2**max_log
  num_elems_arr = np.rint(2**np.arange(0, 11, 1.0/3.0)).astype(int)
  config_arr = [(512, 4), (512, 8), (1024, 2), (1024, 4), (1024, 8)]

  addr_space = list(range(len_addr_space))
  random.shuffle(addr_space)

  plt.figure()
  plt.xlabel("Number of elements ($n$)")
  plt.ylabel("Probability of conflict")
  plt.xscale('log', base=2)
  plt.yscale('linear')

  for config in config_arr:
    print(f"At config {config}")
    len_signature, num_partitions = config
    rates = []
    for num_elems in num_elems_arr:
      rate = get_false_pos_rate(addr_space, num_elems, len_signature, num_partitions)
      rates.append(rate)
    rates = np.array(rates)

    plt.plot(num_elems_arr, rates, '-', label=f'm={len_signature}, k={num_partitions}')
  
  plt.legend()
  plt.show()

if __name__ == "__main__":
  graph_false_pos_rate()