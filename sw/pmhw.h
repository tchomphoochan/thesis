#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cpp_contracts
#include <assert.h>
void contract_assert(bool expr) {
  assert(expr);
}
#endif

/*
Maximum number of objects per transacation
*/
#define PMHW_MAX_TXN_READ_OBJS  8
#define PMHW_MAX_TXN_WRITE_OBJS 8
#define PMHW_MAX_TXN_TOTAL_OBJS 16

/*
Return codes for Puppetmaster-related stuff
*/
typedef enum {
  // Ok results
  PMHW_OK = 0,
  PMHW_PARTIAL = 1,
  // Fail to talk with hardware
  PMHW_NO_HW_CONN = 2,
  // Illegal operation
  PMHW_ILLEGAL_OP = 3,
  // Unsupported values
  PMHW_INVALID_VALS = 4
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
  bool useSimulatedTxnDriver;
  bool useSimulatedPuppets;
  int simulatedPuppetsClockPeriod;
} pmhw_config_t;

/*
Puppetmaster transaction descriptor
*/
typedef struct {
  int transactionId;
  uint64_t auxData;

  int numReadObjs;
  uint64_t readObjIds[PMHW_MAX_TXN_READ_OBJS];

  int numWriteObjs;
  uint64_t writeObjIds[PMHW_MAX_TXN_WRITE_OBJS];
} pmhw_txn_t;


/*
Query the connected FPGA for current configuration values.
Returns PMHW_NO_HW_CONN if cannot talk with hardware.
*/
pmhw_retval_t pmhw_get_config(pmhw_config_t *ret);


/*
Set hardware configuration.
This will load the correct hardware bitstream if available,
otherwise returns PMHW_INVALID_VALS.
*/
pmhw_retval_t pmhw_set_config(const pmhw_config_t *cfg);


/*
Initialize/reset transactions in the hardware.
*/
pmhw_retval_t pmhw_reset();


/*
Ask Puppetmaster to schedule a transaction.
*/
pmhw_retval_t pmhw_schedule(const pmhw_txn_t *txn);


/*
Get a scheduled transaction from Puppetmaster. Returns the transaction ID.
*/
pmhw_retval_t pmhw_poll_scheduled(pmhw_txn_t *ret);

#ifdef __cplusplus
}
#endif
