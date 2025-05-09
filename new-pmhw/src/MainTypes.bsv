import BasicTypes::*;
import Vector::*;

/*
Utility
*/
typedef Bit#(TLog#(size)) TIndex#(numeric type size);
typedef Bit#(TAdd#(1, TLog#(size))) TCount#(numeric type size);

/*
Input transaction into Puppetmaster
*/
Integer maxTxnReadObjs = valueOf(MaxTxnReadObjs);
Integer maxTxnWriteObjs = valueOf(MaxTxnWriteObjs);

typedef struct {
    TransactionId txnId;
    AuxData auxData;
    TCount#(MaxTxnReadObjs) numReadObjs;
    Vector#(MaxTxnReadObjs, ObjectId) readObjs;
    TCount#(MaxTxnWriteObjs) numWriteObjs;
    Vector#(MaxTxnWriteObjs, ObjectId) writeObjs;
} Transaction deriving (Bounded, Bits, Eq, FShow);

typedef 4 NumBloomParts;
typedef 8 NumBloomChunks;
typedef 256 BloomChunkSize;

typedef TIndex#(NumBloomParts) BloomPartIndex;
typedef TIndex#(NumBloomChunks) BloomChunkIndex;
typedef TIndex#(BloomChunkSize) BloomBitIndex;

typedef TMul#(NumBloomChunks, BloomChunkSize) BloomPartSize;
typedef Bit#(BloomChunkSize) BloomChunk;
typedef Vector#(NumBloomParts, Bit#(BloomChunkSize)) BloomChunkParts;

