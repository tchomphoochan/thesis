import BasicTypes::*;
import Vector::*;

/*
Utility
*/
typedef UInt#(TAdd#(1, TLog#(size))) TNum#(numeric type size);

/*
Input transaction into Puppetmaster
*/
Integer maxTxnReadObjs = valueOf(MaxTxnReadObjs);
Integer maxTxnWriteObjs = valueOf(MaxTxnWriteObjs);

typedef struct {
    TransactionId transactionId;
    AuxData auxData;
    TNum#(MaxTxnReadObjs) numReadObjs;
    Vector#(MaxTxnReadObjs, ObjectHash) readObjs;
    TNum#(MaxTxnWriteObjs) numWriteObjs;
    Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs;
} Transaction deriving (Bits, Eq, FShow);

/*
Hash stuff
*/
typedef 4 NumBloomParts;
typedef 4096 BloomPartSize;
typedef Bit#(TLog#(BloomPartSize)) BloomPartIndex;
typedef Vector#(NumBloomParts, BloomPartIndex) ObjectHash;
