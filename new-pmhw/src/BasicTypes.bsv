// Basic types that are meant to be shared across the entire system
// Usually constants.

typedef 8 MaxTxnReadObjs;   // If this is increased, H2S must be adjusted accordingly
typedef 8 MaxTxnWriteObjs;  // If this is increased, H2S must be adjusted accordingly
typedef 64 MaxNumPuppets;

typedef Bit#(32) TransactionId;
typedef Bit#(64) AuxData;
typedef Bit#(64) ObjectId;
typedef Bit#(64) Timestamp;

