#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "pmhw.h"

/*
Connectal-required wrappers
*/
#include "GeneratedTypes.h"
#include "HostSetupRequest.h"
#include "HostTxnRequest.h"
#include "DebugIndication.h"
#include "WorkIndication.h"
#include "HostWorkDone.h"

#define DEBUG_LOG(stuff) \
  std::cerr << "[" << __FILE__ << ":" << __LINE__ << "] " << stuff << std::endl

class DebugIndication : public DebugIndicationWrapper {
public:
  std::queue<PmConfigValues> configVals;
  std::mutex mutex;
  std::condition_variable cv;
  void getPmConfig(PmConfigValues m) {
    std::unique_lock guard(mutex);
    configVals.push(m);
    cv.notify_all();
  }
  void transactionRenamed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " renamed on cycle " << m.endTime);
  }
  void transactionFailed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " failed on cycle " << m.endTime);
  }
  void transactionFreed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " freed on cycle " << m.endTime);
  }
  DebugIndication(int id) : DebugIndicationWrapper(id), configVals(), mutex(), cv() {}
};

class WorkIndication : public WorkIndicationWrapper {
public:
  std::queue<WorkMessage> msgs;
  std::mutex mutex;
  std::condition_variable cv;
  void startWork(WorkMessage m) {
    DEBUG_LOG("T#" << m.tid << " scheduled on cycle " << m.cycle << " for P#" << m.pid);
    std::unique_lock guard(mutex);
    msgs.push(m);
    cv.notify_all();
  }
  WorkIndication(int id) : WorkIndicationWrapper(id), msgs(), mutex(), cv() {}
};

/*
Singleton representing active Puppetmaster instance
*/
static struct pmhw_singleton_t {
  bool initialized = false;
  pmhw_config_t cached_config;
  std::unique_ptr<HostSetupRequestProxy> setup = nullptr;
  std::unique_ptr<HostTxnRequestProxy> txn = nullptr;
  std::unique_ptr<HostWorkDoneProxy> workDone = nullptr;
  std::unique_ptr<DebugIndication> debugInd = nullptr;
  std::unique_ptr<WorkIndication> workInd = nullptr;
} pmhw;

/*
Interfaces
*/

pmhw_retval_t pmhw_reset() {
  pmhw.initialized = true;
  pmhw.setup = std::make_unique<HostSetupRequestProxy>(IfcNames_HostSetupRequestS2H);
  pmhw.txn = std::make_unique<HostTxnRequestProxy>(IfcNames_HostTxnRequestS2H);
  pmhw.workDone = std::make_unique<HostWorkDoneProxy>(IfcNames_HostWorkDoneS2H);
  pmhw.debugInd = std::make_unique<DebugIndication>(IfcNames_DebugIndicationH2S);
  pmhw.workInd = std::make_unique<WorkIndication>(IfcNames_WorkIndicationH2S);
  pmhw.txn->clearState();
  return PMHW_OK;
}

pmhw_retval_t pmhw_get_config(pmhw_config_t *ret) {
  contract_assert(pmhw.initialized);
  pmhw.setup->fetchConfig();

  std::unique_lock guard(pmhw.debugInd->mutex);
  pmhw.debugInd->cv.wait(guard, [] {
    return !pmhw.debugInd->configVals.empty();
  });
  auto configVals = pmhw.debugInd->configVals.front();
  pmhw.debugInd->configVals.pop();

  ret->logNumberRenamerThreads = configVals.logNumberRenamerThreads;
  ret->logNumberShards = configVals.logNumberShards;
  ret->logSizeShard = configVals.logSizeShard;
  ret->logNumberHashes = configVals.logNumberHashes;
  ret->logNumberComparators = configVals.logNumberComparators;
  ret->logNumberSchedulingRounds = configVals.logNumberSchedulingRounds;
  ret->logNumberPuppets = configVals.logNumberPuppets;
  ret->numberAddressOffsetBits = configVals.numberAddressOffsetBits;
  ret->logSizeRenamerBuffer = configVals.logSizeRenamerBuffer;
  ret->useSimulatedTxnDriver = configVals.useSimulatedTxnDriver;
  ret->useSimulatedPuppets = configVals.useSimulatedPuppets;
  ret->simulatedPuppetsClockPeriod = configVals.simulatedPuppetsClockPeriod;

  pmhw.cached_config = *ret;
  return PMHW_OK;
}

pmhw_retval_t pmhw_set_config(const pmhw_config_t *cfg) {
  contract_assert(pmhw.initialized);
  pmhw.setup->setTxnDriver(cfg->useSimulatedTxnDriver);
  pmhw.setup->setSimulatedPuppets(cfg->useSimulatedPuppets, cfg->simulatedPuppetsClockPeriod);
  pmhw.cached_config = *cfg;
  return PMHW_PARTIAL;
}

pmhw_retval_t pmhw_schedule(const pmhw_txn_t *txn) {
  contract_assert(pmhw.initialized);
  contract_assert(txn->numReadObjs <= PMHW_MAX_TXN_READ_OBJS);
  contract_assert(txn->numWriteObjs <= PMHW_MAX_TXN_WRITE_OBJS);
  pmhw.txn->enqueueTransaction(
    txn->transactionId,
    txn->auxData,
    txn->numReadObjs,
    txn->readObjIds[0], txn->readObjIds[1], txn->readObjIds[2], txn->readObjIds[3],
    txn->readObjIds[4], txn->readObjIds[5], txn->readObjIds[6], txn->readObjIds[7],
    txn->numWriteObjs,
    txn->writeObjIds[0], txn->writeObjIds[1], txn->writeObjIds[2], txn->writeObjIds[3],
    txn->writeObjIds[4], txn->writeObjIds[5], txn->writeObjIds[6], txn->writeObjIds[7]
  );
  return PMHW_OK;
}

pmhw_retval_t pmhw_trigger_simulated_driver() {
  contract_assert(pmhw.initialized);
  contract_assert(pmhw.cached_config.useSimulatedTxnDriver);
  pmhw.txn->trigger();
  return PMHW_OK;
}

pmhw_retval_t pmhw_force_trigger_scheduling() {
  contract_assert(pmhw.initialized);
  pmhw.txn->clearState();
  return PMHW_OK;
}

pmhw_retval_t pmhw_poll_scheduled(int *transactionId, int *puppetId) {
  contract_assert(pmhw.initialized);
  contract_assert(!pmhw.cached_config.useSimulatedPuppets);

  std::unique_lock guard(pmhw.workInd->mutex);
  if (!pmhw.workInd->cv.wait_for(guard, std::chrono::milliseconds(100), [] {
    return !pmhw.workInd->msgs.empty();
  })) {
    return PMHW_TIMEOUT;
  }

  *transactionId = pmhw.workInd->msgs.front().tid;
  *puppetId = pmhw.workInd->msgs.front().pid;
  pmhw.workInd->msgs.pop();
  return PMHW_OK;
}

pmhw_retval_t pmhw_report_done(int transactionId, int puppetId) {
  (void)transactionId; // unused
  contract_assert(pmhw.initialized);
  contract_assert(!pmhw.cached_config.useSimulatedPuppets);
  pmhw.workDone->workDone(puppetId);
  return PMHW_OK;
}

