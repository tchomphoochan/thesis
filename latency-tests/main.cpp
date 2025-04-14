#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <cmath>

#include <semaphore.h>
#include "GeneratedTypes.h"
#include "PingPongRequest.h"
#include "PingPongIndication.h"

#define NUM_SAMPLES (100000)

sem_t sem;
double hw_report;

class PingPongIndication : public PingPongIndicationWrapper {
  int n = 0;
  void pong() {
    if (n == 0) sem_post(&sem);
    else n--;
  }
  void reportTime(Cycle duration) {
    hw_report = (double)duration;
    sem_post(&sem);
  }

public:
  PingPongIndication(int id) : PingPongIndicationWrapper(id) {}
  void setCountdown(int n) {
    this->n = n;
  }
};

int main() {
  PingPongRequestProxy *ppReq = new PingPongRequestProxy(IfcNames_PingPongRequestS2H);
  PingPongIndication ppInd(IfcNames_PingPongIndicationH2S);

  std::cout << std::fixed;
  std::cout << std::setprecision(9);

  // Test 0: Ping-pong latency
  {
    double sum = 0, sumsq = 0;
    for (int sample = 0; sample < NUM_SAMPLES; ++sample) {
      const auto t0 = std::chrono::steady_clock::now();
      ppReq->ping(true);
      sem_wait(&sem);
      const auto t1 = std::chrono::steady_clock::now();
      const std::chrono::duration<double> d = t1-t0;
      sum += d.count();
      sumsq += d.count() * d.count();
    }
    double avg = sum/NUM_SAMPLES;
    double avgsq = sumsq/NUM_SAMPLES;
    double var = avgsq - avg*avg;
    double sd = std::sqrt(var);
    std::cout << "Ping-pong latency mean: " << avg << "s" << std::endl;
    std::cout << "Ping-pong latency SD: " << sd << "s" << std::endl;
  }

  // Test 1: Ping-only throughput
  {
    const auto t0 = std::chrono::steady_clock::now();
    for (int sample = 0; sample < NUM_SAMPLES; ++sample) {
      ppReq->ping(false);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::chrono::duration<double> d = t1-t0;
    double tp = NUM_SAMPLES / d.count();
    std::cout << "Ping-only throughput: " << tp << "/s" << std::endl;
  }

  // Test 2: Pong-only throughput
  {
    ppInd.setCountdown(NUM_SAMPLES-1);
    const auto t0 = std::chrono::steady_clock::now();
    ppReq->start(NUM_SAMPLES);
    sem_wait(&sem);
    const auto t1 = std::chrono::steady_clock::now();
    const std::chrono::duration<double> d = t1-t0;
    double tp = (NUM_SAMPLES) / d.count();
    std::cout << "Pong-only throughput: " << tp << "/s" << std::endl;
    sem_wait(&sem);
    std::cout << "  Hardware throughput: " << ((double)NUM_SAMPLES/hw_report) << "/cycle" << std::endl;
  }

  // Test 3: Full duplex throughput
  {
    const auto t0 = std::chrono::steady_clock::now();
    ppInd.setCountdown(NUM_SAMPLES-1);
    for (int sample = 0; sample < NUM_SAMPLES; ++sample) {
      ppReq->ping(true);
    }
    sem_wait(&sem);
    const auto t1 = std::chrono::steady_clock::now();
    const std::chrono::duration<double> d = t1-t0;
    double tp = NUM_SAMPLES / d.count();
    std::cout << "Ping-pong throughput: " << tp << "/s" << std::endl;
  }

  return 0;
}
