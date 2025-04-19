from copy import copy
from typing import *
from bitarray import bitarray
from dataclasses import dataclass
import random
import abc

class Set(abc.ABC):
  def add(self, elem: int):
    raise NotImplementedError

  def __contains__(self, elem: int) -> bool:
    raise NotImplementedError

  def __and__(self, other: Self) -> Self:
    raise NotImplementedError

  def __or__(self, other: Self) -> Self:
    raise NotImplementedError

  def remove(self, elem: int):
    raise NotImplementedError

  def __len__(self) -> int:
    raise NotImplementedError

  def __copy__(self) -> Self:
    raise NotImplementedError


@dataclass
class BloomFilter(Set):
  bits: bitarray
  hash_fns: list[Callable[[int], int]]

  def add(self, elem: int):
    for fn in self.hash_fns:
      self.bits[fn(elem)] = 1

  def __contains__(self, elem: int) -> bool:
    return all((self.bits[fn(elem)] for fn in self.hash_fns))

  def __and__(self, other: Self) -> Self:
    assert isinstance(other, BloomFilter)
    assert all((f1 == f2 for f1, f2 in zip(self.hash_fns, other.hash_fns)))
    assert self.len() == other.len()
    return BloomFilter(bits=self.bits & other.bits, hash_fns=self.hash_fns)

  def __or__(self, other: Self) -> Self:
    assert isinstance(other, BloomFilter)
    assert all((f1 == f2 for f1, f2 in zip(self.hash_fns, other.hash_fns)))
    assert len(self.bits) == len(other.bits)
    return BloomFilter(bits=self.bits | other.bits, hash_fns=self.hash_fns)

  def remove(self, elem: int):
    raise Exception("Bloom filter does not support removal")

  def __len__(self) -> int:
    raise Exception("Bloom filter does not support length operation")

  def __copy__(self) -> Self:
    return BloomFilter(bits=copy(self.bits), hash_fns=self.hash_fns)


@dataclass
class ParallelBloomFilter(Set):
  parts: list[BloomFilter]

  def add(self, elem: int):
    for part in self.parts:
      part.add(elem)

  def __contains__(self, elem: int) -> bool:
    return all(elem in part for part in self.parts)

  def __and__(self, other: Self) -> Self:
    assert isinstance(other, ParallelBloomFilter)
    assert len(self.parts) == len(other.parts)
    return ParallelBloomFilter(parts=[p1&p2 for p1, p2 in zip(self.parts, other.parts)])

  def __or__(self, other: Self) -> Self:
    assert isinstance(other, ParallelBloomFilter)
    assert len(self.parts) == len(other.parts)
    return ParallelBloomFilter(parts=[p1|p2 for p1, p2 in zip(self.parts, other.parts)])

  def remove(self, elem: int):
    raise Exception("Parallel bloom filter does not support removal")

  def __len__(self) -> int:
    raise Exception("Parallel bloom filter does not support length operation")

  def __copy__(self) -> Self:
    return ParallelBloomFilter(parts=[part.copy() for part in self.parts])


def make_hash_function(buckets):
  # TODO: actually make this more reasonable
  mult = (random.randint(2**40, 2**60))*2 + 1
  def f(x):
    return (x * mult) // 2**30 % buckets
  return f


def make_bloom_filter_family(len_signature: int, num_hashes: int) -> Callable[[], BloomFilter]:
  hash_fns = [make_hash_function(len_signature) for _ in range(num_hashes)]
  return lambda: BloomFilter(bits=bitarray(len_signature), hash_fns=hash_fns)


def make_bloom_filter(len_signature: int, num_hashes: int) -> BloomFilter:
  return make_bloom_filter_family(len_signature, num_hashes)()


def make_parallel_bloom_filter_family(len_signature: int, num_partitions: int) -> Callable[[], ParallelBloomFilter]:
  assert len_signature % num_partitions == 0
  len_per_part = len_signature // num_partitions
  part_families = [make_bloom_filter_family(len_per_part, 1) for _ in range(num_partitions)]
  return lambda: ParallelBloomFilter(parts=[part_families[i]() for i in range(num_partitions)])


def make_parallel_bloom_filter(len_signature: int, num_partitions: int) -> ParallelBloomFilter:
  return make_parallel_bloom_filter_family(len_signature, num_partitions)()

