#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <x86intrin.h>
#include <inttypes.h>

#include "spsc_queue.h"

#define QUEUE_CAPACITY 1024
#define BENCH_DURATION_SEC 5
#define LATENCY_SAMPLE_MAX 1000000
#define PRODUCER_CORE 1
#define CONSUMER_CORE 2

int payload_sizes[] = {8, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
const int NUM_SIZES = sizeof(payload_sizes) / sizeof(payload_sizes[0]);

typedef struct {
  void *buffer;
  int size;
  ulong produced;
  ulong consumed;
  uint64_t *latencies;
  uint64_t latency_index;
} BenchContext;

typedef struct {
  void *data;
  uint64_t timestamp;
} GenericItem;

SPSC_QUEUE_IMPL(GenericItem, GenericQueue, GenericQueue)
GenericQueue queue;
static volatile bool running = true;
static double cpu_ghz = 1.0;

void pin_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void print_with_commas(uint64_t n) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%" PRIu64, n);
  int len = strlen(buf);
  int commas = (len - 1) / 3;
  int out_len = len + commas;
  char out[32];
  out[out_len] = '\0';

  int j = out_len - 1;
  int count = 0;
  for (int i = len - 1; i >= 0; --i) {
    if (count == 3) {
      out[j--] = ',';
      count = 0;
    }
    out[j--] = buf[i];
    count++;
  }
  printf("%s", out);
}

void *producer_thread_tp(void *arg) {
  BenchContext *ctx = (BenchContext *)arg;
  pin_to_core(PRODUCER_CORE);
  GenericItem item;
  item.data = ctx->buffer;

  while (running) {
    item.timestamp = 0;
    if (GenericQueue_enq(&queue, &item)) {
      ctx->produced++;
    }
  }
  return NULL;
}

void *consumer_thread_tp(void *arg) {
  BenchContext *ctx = (BenchContext *)arg;
  pin_to_core(CONSUMER_CORE);
  GenericItem item;

  while (running) {
    if (GenericQueue_deq(&queue, &item)) {
      ctx->consumed++;
    }
  }
  return NULL;
}

void run_throughput_test(int payload_size) {
  BenchContext ctx = {.size = payload_size};
  ctx.buffer = malloc(payload_size);
  ctx.produced = 0;
  ctx.consumed = 0;

  GenericQueue_init(&queue, QUEUE_CAPACITY);
  running = true;

  pthread_t producer, consumer;
  pthread_create(&producer, NULL, producer_thread_tp, &ctx);
  pthread_create(&consumer, NULL, consumer_thread_tp, &ctx);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  sleep(BENCH_DURATION_SEC);
  running = false;
  pthread_join(producer, NULL);
  pthread_join(consumer, NULL);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double duration_sec = (end.tv_sec - start.tv_sec) +
                        (end.tv_nsec - start.tv_nsec) / 1e9;

  uint64_t consumed_bits = (uint64_t)ctx.consumed * payload_size * 8;

  printf("=== Throughput Test: %4d bytes ===\n", payload_size);
  printf("Duration            : %.2f sec\n", duration_sec);
  printf("Consumed            : ");
  print_with_commas(ctx.consumed);
  printf(" items (");
  print_with_commas(consumed_bits / (uint64_t)duration_sec);
  printf(" bits/sec)\n");
  printf("Loss (enqueue fail) : %.2f%%\n\n", 100.0 * (ctx.produced - ctx.consumed) / (ctx.produced + 1));

  GenericQueue_free(&queue);
  free(ctx.buffer);
}

void *producer_thread_lat(void *arg) {
  BenchContext *ctx = (BenchContext *)arg;
  pin_to_core(PRODUCER_CORE);
  GenericItem item;
  item.data = ctx->buffer;

  while (running) {
    unsigned aux;
    item.timestamp = __rdtscp(&aux);
    if (GenericQueue_enq(&queue, &item)) {
      ctx->produced++;
    }
  }
  return NULL;
}

void *consumer_thread_lat(void *arg) {
  BenchContext *ctx = (BenchContext *)arg;
  pin_to_core(CONSUMER_CORE);
  GenericItem item;

  while (running) {
    if (GenericQueue_deq(&queue, &item)) {
      unsigned aux;
      uint64_t now = __rdtscp(&aux);
      uint64_t lat = now - item.timestamp;

      uint64_t idx = ctx->latency_index++;
      if (idx < LATENCY_SAMPLE_MAX)
        ctx->latencies[idx] = lat;

      ctx->consumed++;
    }
  }
  return NULL;
}

int compare_u64(const void *a, const void *b) {
  uint64_t x = *(uint64_t *)a;
  uint64_t y = *(uint64_t *)b;
  return (x > y) - (x < y);
}

void print_latency_stats(uint64_t *samples, size_t count) {
  if (count == 0) {
    printf("No latency samples collected.\n\n");
    return;
  }

  qsort(samples, count, sizeof(uint64_t), compare_u64);

  uint64_t min = samples[0];
  uint64_t max = samples[count - 1];
  uint64_t avg = 0;
  for (size_t i = 0; i < count; ++i)
    avg += samples[i];
  avg /= count;

  uint64_t p50 = samples[count * 50 / 100];
  uint64_t p99 = samples[count * 99 / 100];
  uint64_t p999 = samples[count * 999 / 1000];

  printf("Latency (cycles)    : min=");
  print_with_commas(min);
  printf(", max=");
  print_with_commas(max);
  printf(", avg=");
  print_with_commas(avg);
  printf("\n");

  printf("Latency (ns est)    : p50=%.1f ns, p99=%.1f ns, p999=%.1f ns\n\n",
         p50 / cpu_ghz, p99 / cpu_ghz, p999 / cpu_ghz);
}

void run_latency_test(int payload_size) {
  BenchContext ctx = {.size = payload_size};
  ctx.buffer = malloc(payload_size);
  ctx.latencies = malloc(sizeof(uint64_t) * LATENCY_SAMPLE_MAX);
  ctx.latency_index = 0;
  ctx.produced = 0;
  ctx.consumed = 0;

  GenericQueue_init(&queue, QUEUE_CAPACITY);
  running = true;

  pthread_t producer, consumer;
  pthread_create(&producer, NULL, producer_thread_lat, &ctx);
  pthread_create(&consumer, NULL, consumer_thread_lat, &ctx);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  sleep(BENCH_DURATION_SEC);
  running = false;
  pthread_join(producer, NULL);
  pthread_join(consumer, NULL);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double duration_sec = (end.tv_sec - start.tv_sec) +
                        (end.tv_nsec - start.tv_nsec) / 1e9;
  uint64_t consumed_bits = ctx.consumed * payload_size * 8;

  uint64_t count = ctx.latency_index;
  if (count > LATENCY_SAMPLE_MAX)
    count = LATENCY_SAMPLE_MAX;

  printf("=== Latency Test:    %4d bytes ===\n", payload_size);
  printf("Duration             : %.2f sec\n", duration_sec);
  printf("Consumed             : ");
  print_with_commas(ctx.consumed);
  printf(" items (");
  print_with_commas(consumed_bits / (uint64_t)duration_sec);
  printf(" bits/sec)\n");
  printf("Loss (enqueue fail)  : %.2f%%\n", 100.0 * (ctx.produced - ctx.consumed) / (ctx.produced + 1));

  print_latency_stats(ctx.latencies, count);

  GenericQueue_free(&queue);
  free(ctx.buffer);
  free(ctx.latencies);
}

double measure_cpu_ghz() {
  struct timespec t0, t1;
  unsigned aux;
  uint64_t c0 = __rdtscp(&aux);
  clock_gettime(CLOCK_MONOTONIC, &t0);
  usleep(100000);
  uint64_t c1 = __rdtscp(&aux);
  clock_gettime(CLOCK_MONOTONIC, &t1);

  double elapsed_sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  return (c1 - c0) / (elapsed_sec * 1e9);
}

int main() {
  cpu_ghz = measure_cpu_ghz();
  printf("SPSC Queue Benchmark (Throughput + Latency)\n");
  printf("Estimated CPU clock : %.3f GHz\n", cpu_ghz);
  printf("Queue capacity      : %d\n", QUEUE_CAPACITY);
  printf("Run time per test   : %d sec\n\n", BENCH_DURATION_SEC);

  for (int i = 0; i < NUM_SIZES; ++i)
    run_throughput_test(payload_sizes[i]);

  for (int i = 0; i < NUM_SIZES; ++i)
    run_latency_test(payload_sizes[i]);

  return 0;
}

