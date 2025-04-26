#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>

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
  void transactionRenamed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " renamed on cycle " << m.endTime);
  }
  void transactionFailed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " failed on cycle " << m.endTime);
  }
  void transactionFreed(DebugMessage m) {
    DEBUG_LOG("T#" << m.tid << " freed on cycle " << m.endTime);
  }
  DebugIndication(int id) : DebugIndicationWrapper(id) {}
};

class WorkIndication : public WorkIndicationWrapper {
public:
  std::queue<WorkMessage> msgs;
  std::mutex mutex;
  std::condition_variable cv;

  void startWork(WorkMessage m) {
    DEBUG_LOG("T#" << m.tid << " scheduled on cycle " << m.cycle << " for P#" << m.pid);
    std::unique_lock workMsgGuard(pmhw.workInd->mutex);
    msgs.push(m);
    cv.notify_all();
  }

  WorkIndication(int id) : WorkIndicationWrapper(id), msgs(), mutex(), cv() {}
};

/*
Singleton representing active Puppetmaster instance
*/
struct {
  bool initialized = false;
  std::unique_ptr<HostSetupRequestProxy> setup;
  std::unique_ptr<HostTxnRequestProxy> txn;
  std::unique_ptr<DebugIndication> debugInd;
  std::unique_ptr<WorkIndication> workInd;
} pmhw;

/*
Interfaces
*/

pmhw_retval_t pmhw_reset() {
  pmhw.initialized = true;
  pmhw.setup = std::make_unique<HostSetupRequestProxy>(IfcNames_HostSetupRequestS2H);
  pmhw.txn = std::make_unique<HostTxnRequestProxy>(IfcNames_HostTxnRequestS2H);
  pmhw.debugIndication = std::make_unique<DebugIndication>(IfcNames_DebugIndicationH2S);
  pmhw.workInd = std::make_unique<WorkIndication>(IfcNames_WorkIndicationH2S);
  pmhw.setup->stopFakeTxnDriver();
  pmhw.txn->clearState();
  return PMHW_OK;
}

pmhw_retval_t pmhw_get_config(pmhw_config_t *ret) {
  contract_assert(pmhw.initialized);

  // TODO: actually aet config from hardware directly
  ret->logNumberRenamerThreads      = 2;
  ret->logNumberShards              = 2;
  ret->logSizeShard                 = 6;
  ret->logNumberHashes              = 6;
  ret->logNumberComparators         = 1;
  ret->logNumberSchedulingRounds    = 1;
  ret->logNumberPuppets             = 3;
  ret->numberAddressOffsetBits      = 6;
  ret->logSizeRenamerBuffer         = 7;
  ret->useSimulatedTxnDriver        = false;
  ret->useSimulatedPuppets          = false;
  ret->simulatedPuppetsClockPeriod  = 20;
}

pmhw_retval_t pmhw_set_config(const pmhw_config_t *cfg) {
  contract_assert(pmhw.initialized);
  pmhw.setup->setTxnDriver(cfg->useSimulatedTxnDriver);
  pmhw.setup->setSimulatedPuppets(cfg->useSimulatedPuppets, cfg->simulatedPuppetsClockPeriod);
  return PMHW_PARTIAL;
}

pmhw_retval_t pmhw_schedule(const pmhw_txn_t *txn) {
  contract_assert(pmhw.initialized);
  pmhw.txn->enqueueTransaction(
    txn.transactionId,
    txn.auxData,
    txn.numReadObjs,
    txn.readObjIds[0], txn.readObjIds[1], txn.readObjIds[2], txn.readObjIds[3],
    txn.readObjIds[4], txn.readObjIds[5], txn.readObjIds[6], txn.readObjIds[7],
    txn.numWriteObjs,
    txn.writeObjIds[0], txn.writeObjIds[1], txn.writeObjIds[2], txn.writeObjIds[3],
    txn.writeObjIds[4], txn.writeObjIds[5], txn.writeObjIds[6], txn.writeObjIds[7]);
  return PMHW_OK;
}

pmhw_retval_t pmhw_poll_scheduled(int *ret) {
  contract_assert(pmhw.initialized);
  std::unique_lock workMsgGuard(pmhw.workInd->mutex);
  pmhw.workInd->cv.wait(workMsgGuard, [] {
    return !msgs.size();
  });
  *ret = pmhw.workInd->msgs.first().tid;
  pmhw.workInd->msgs.pop();
  return PMHW_OK;
}

