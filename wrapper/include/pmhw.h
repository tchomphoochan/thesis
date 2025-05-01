#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cpp_contracts
#include <assert.h>
static inline void contract_assert(bool expr) {
  assert(expr);
}
#endif

/*
Maximum number of objects per transaction
*/
#define PMHW_MAX_TXN_READ_OBJS  16
#define PMHW_MAX_TXN_WRITE_OBJS 16
#define PMHW_MAX_TXN_TOTAL_OBJS 16

/*
Return codes for Puppetmaster-related operations
*/
typedef enum {
  // Ok results
  PMHW_OK = 0,
  PMHW_PARTIAL = 1,
  // Operation timed out (e.g., no transaction scheduled within timeout window)
  PMHW_TIMEOUT = 2,
  // Fail to talk with hardware
  PMHW_NO_HW_CONN = 3,
  // Illegal or unsupported operation
  PMHW_ILLEGAL_OP = 4,
  // Unsupported or invalid configuration values
  PMHW_INVALID_VALS = 5
} pmhw_retval_t;

/*
Puppetmaster hardware configuration
*/
typedef struct {
  int logNumberRenamerThreads;
  int logNumberShards;
  int logSizeShard;
  int logNumberHashes;
  int logNumberComparators;
  int logNumberSchedulingRounds;
  int logNumberPuppets;
  int numberAddressOffsetBits;
  int logSizeRenamerBuffer;
  bool useSimulatedTxnDriver;    // If true, use synthetic transaction driver inside hardware
  bool useSimulatedPuppets;      // If true, puppets will self-complete work automatically
  int simulatedPuppetsClockPeriod; // Clock period for simulated puppets (only relevant if useSimulatedPuppets is true)
} pmhw_config_t;

/*
Puppetmaster transaction descriptor
*/
typedef struct {
  int transactionId;                     // Application-defined transaction ID
  uint64_t auxData;                       // User-defined auxiliary data
  int numReadObjs;                        // Number of read objects
  uint64_t readObjIds[PMHW_MAX_TXN_READ_OBJS];   // List of object IDs read
  int numWriteObjs;                       // Number of write objects
  uint64_t writeObjIds[PMHW_MAX_TXN_WRITE_OBJS]; // List of object IDs written
} pmhw_txn_t;

/*
Interfaces
*/

/*
Fetch the current hardware configuration from Puppetmaster.
Blocks until configuration is available.
Returns PMHW_OK on success, or PMHW_NO_HW_CONN if hardware unavailable.
*/
pmhw_retval_t pmhw_get_config(pmhw_config_t *ret);

/*
Request a change to the Puppetmaster hardware configuration.
Only a limited subset of options may be set (e.g., switching to simulation modes).
Returns PMHW_PARTIAL on partial success, or PMHW_INVALID_VALS if invalid.
*/
pmhw_retval_t pmhw_set_config(const pmhw_config_t *cfg);

/*
Reset the Puppetmaster hardware state and reinitialize internal structures.
Must be called before any other operations.
Returns PMHW_OK or PMHW_NO_HW_CONN.
*/
pmhw_retval_t pmhw_reset();

/*
Submit a new transaction descriptor to Puppetmaster.
The transaction may be buffered internally or immediately scheduled depending on system load.
Returns PMHW_OK or PMHW_NO_HW_CONN.
*/
pmhw_retval_t pmhw_schedule(const pmhw_txn_t *txn);

/*
Trigger all enqueued transactions to be submitted to the scheduling core.
Only relevant if using a simulated transaction driver.
Returns PMHW_OK or PMHW_NO_HW_CONN.
*/
pmhw_retval_t pmhw_trigger_simulated_driver();

/*
Force the scheduler to attempt scheduling immediately.
Useful when system is idle but fewer than the normal batch size of transactions are available.
Returns PMHW_OK or PMHW_ILLEGAL_OP if unsupported.
*/
pmhw_retval_t pmhw_force_trigger_scheduling();

/*
Poll for a scheduled transaction assigned to a puppet.
If a transaction becomes ready, fills in transactionId and puppetId.
May return PMHW_TIMEOUT if no transaction is ready within a timeout window.
Returns PMHW_OK on success.
*/
pmhw_retval_t pmhw_poll_scheduled(int *transactionId);

/*
Report that a previously assigned transaction has been completed by a puppet.
This signals the scheduler that the puppet is now idle and ready for new work.
Returns PMHW_OK or PMHW_ILLEGAL_OP if called improperly.
*/
pmhw_retval_t pmhw_report_done(int transactionId);

#ifdef __cplusplus
}
#endif

