#include <queue>
#include <mutex>
#include <condition_variable>

#include "pmhw.h"
#include "pmutils.h"

/*
Connectal-required wrappers
*/
#include "GeneratedTypes.h"
#include "HostSetupRequest.h"
#include "HostTxnRequest.h"
#include "DebugIndication.h"
#include "WorkIndication.h"
#include "HostWorkDone.h"

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
    // DEBUG("T#%lu renamed on cycle %lu", m.tid, m.endTime);
  }
  void transactionFailed(DebugMessage m) {
    // DEBUG("T#%lu failed on cycle %lu", m.tid, m.endTime);
  }
  void transactionFreed(DebugMessage m) {
    // DEBUG("T#%lu freed on cycle %lu", m.tid, m.endTime);
  }
  DebugIndication(int id) : DebugIndicationWrapper(id), configVals(), mutex(), cv() {}
};

class WorkIndication : public WorkIndicationWrapper {
public:
  std::queue<WorkMessage> msgs;
  std::mutex mutex;
  void startWork(WorkMessage m) {
    // DEBUG_LOG("T#" << m.tid << " scheduled on cycle " << m.cycle << " for P#" << m.pid);
    std::unique_lock guard(mutex);
    msgs.push(m);
  }
  WorkIndication(int id) : WorkIndicationWrapper(id), msgs(), mutex() {}
};

/*
Singleton representing active Puppetmaster instance
*/
static struct pmhw_singleton_t {
  bool initialized = false;
  std::unique_ptr<HostSetupRequestProxy> setup = nullptr;
  std::unique_ptr<HostTxnRequestProxy> txn = nullptr;
  std::unique_ptr<HostWorkDoneProxy> workDone = nullptr;
  std::unique_ptr<DebugIndication> debugInd = nullptr;
  std::unique_ptr<WorkIndication> workInd = nullptr;
} pmhw;

/*
Interfaces
*/

void pmhw_init(int num_clients, int num_puppets) {
  pmhw.initialized = true;
  pmhw.setup = std::make_unique<HostSetupRequestProxy>(IfcNames_HostSetupRequestS2H);
  pmhw.txn = std::make_unique<HostTxnRequestProxy>(IfcNames_HostTxnRequestS2H);
  pmhw.workDone = std::make_unique<HostWorkDoneProxy>(IfcNames_HostWorkDoneS2H);
  pmhw.debugInd = std::make_unique<DebugIndication>(IfcNames_DebugIndicationH2S);
  pmhw.workInd = std::make_unique<WorkIndication>(IfcNames_WorkIndicationH2S);
  pmhw.txn->clearState();
}

void pmhw_shutdown() {}

void pmhw_schedule(int client_id, const txn_t *txn) {
  ASSERT(pmhw.initialized);
  ASSERT(txn->num_objs <= MAX_TXN_OBJS);
  // TODO:
  // pmhw.txn->enqueueTransaction(
  //   txn->transactionId,
  //   txn->auxData,
  //   txn->numReadObjs,
  //   txn->readObjIds[0], txn->readObjIds[1], txn->readObjIds[2], txn->readObjIds[3],
  //   txn->readObjIds[4], txn->readObjIds[5], txn->readObjIds[6], txn->readObjIds[7],
  //   txn->numWriteObjs,
  //   txn->writeObjIds[0], txn->writeObjIds[1], txn->writeObjIds[2], txn->writeObjIds[3],
  //   txn->writeObjIds[4], txn->writeObjIds[5], txn->writeObjIds[6], txn->writeObjIds[7]
  // );
}

bool pmhw_poll_scheduled(int puppet_id, txn_id_t *txn_id) {
  // TODO
  return false;
}

void pmhw_report_done(int puppet_id, txn_id_t txn_id) {
  // TODO
}

