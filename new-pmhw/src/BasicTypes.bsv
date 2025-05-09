// Basic types that are meant to be shared across the entire system
// Usually constants.

typedef 4 MaxTxnReadObjs;   // If this is increased, H2S must be adjusted accordingly
typedef 4 MaxTxnWriteObjs;  // If this is increased, H2S must be adjusted accordingly
typedef 8 MaxNumPuppets;

typedef Bit#(32) TransactionId;
typedef Bit#(64) AuxData;
typedef Bit#(32) ObjectId;
typedef Bit#(64) Timestamp;

