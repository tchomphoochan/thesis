import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import ClientServer::*;
import BRAM::*;
import UniqueWrappers::*;

/*
In general, only one operation at a time can be processed.
Request ports will block if there is an incomplete operation.
*/
interface Summary;
    // Add a transaction to the summary.
    interface Put#(Transaction) txns;

    // Check whether a transaction is compatible with the summary or not.
    interface Server#(Transaction, Bool) checks;

    // Switch to copy mode. Only succeeds when no other operations are in progress.
    // In copy mode, one chunk at a time is returned and cleared from the BRAM.
    // This should take up to NumBloomPartChunks cycles.
    method Action startClearCopy;
    interface Get#(BloomChunkParts) getChunk;
    interface Put#(BloomChunkParts) setChunk;

    // Return true if can accept operation.
    method Bool isFree;
endinterface

// Hash constants for different hash functions
// Using prime numbers for good distribution
typedef Bit#(32) HashValue;
typedef Vector#(NumBloomParts, HashValue) HashValues;
function HashValue hash(ObjectId obj, Bit#(32) constant);
    return truncate(zeroExtend(obj) * constant);
endfunction

// Generate all hash values for an object
function HashValues hashObject(ObjectId obj);
    Vector#(NumBloomParts, Bit#(32)) constants = newVector;
    constants[0] = 32'h9e3779b1; // Prime numbers and golden ratio related constants
    constants[1] = 32'h85ebca77; // for better distribution
    constants[2] = 32'hc2b2ae3d;
    constants[3] = 32'h27d4eb2f;
    
    Vector#(NumBloomParts, HashValue) result = newVector;
    for (Integer i = 0; i < valueOf(NumBloomParts); i = i + 1) begin
        result[i] = hash(obj, constants[i]);
    end
    return result;
endfunction

function BloomChunkIndex getChunkIndex(HashValue h);
    TIndex#(BloomPartSize) chunkAndBit = truncate(h);
    return truncateLSB(h);
endfunction

function BloomBitIndex getBitIndex(HashValue h);
    return truncate(h);
endfunction

// Add object to the bloom filter chunks
function Tuple2#(Bool, BloomChunkParts) checkOrAddToChunk(BloomChunkParts parts, BloomChunkIndex currentChunk, Transaction txn);
    // We'll modify a copy of the input parts
    BloomChunkParts res = parts;

    Bool isCompatible = True;
    
    // Process all write objects
    for (Integer i = 0; i < valueOf(MaxTxnWriteObjs); i = i + 1) begin
        if (fromInteger(i) < txn.numWriteObjs) begin
            ObjectId obj = txn.writeObjs[i];
            HashValues hashes = hashObject(obj);
            
            // For each hash function
            for (Integer h = 0; h < valueOf(NumBloomParts); h = h + 1) begin
                BloomChunkIndex chunkIdx = getChunkIndex(hashes[h]);
                BloomBitIndex bitIndex = getBitIndex(hashes[h]);
                
                // Only modify the current chunk we're processing
                if (chunkIdx == currentChunk) begin
                    if (parts[h][bitIndex] == 1'b1) begin
                        isCompatible = False;
                    end
                    res[h][bitIndex] = 1'b1;
                end
            end
        end
    end
    
    // Also process all read objects (as per your request to treat all objects as potential conflicts)
    for (Integer i = 0; i < valueOf(MaxTxnReadObjs); i = i + 1) begin
        if (fromInteger(i) < txn.numReadObjs) begin
            ObjectId obj = txn.readObjs[i];
            HashValues hashes = hashObject(obj);
            
            // For each hash function
            for (Integer h = 0; h < valueOf(NumBloomParts); h = h + 1) begin
                BloomChunkIndex chunkIdx = getChunkIndex(hashes[h]);
                BloomBitIndex bitIndex = getBitIndex(hashes[h]);
                
                // Only modify the current chunk we're processing
                if (chunkIdx == currentChunk) begin
                    if (parts[h][bitIndex] == 1'b1) begin
                        isCompatible = False;
                    end
                    res[h][bitIndex] = 1'b1;
                end
            end
        end
    end
    
    return tuple2(isCompatible, res);
endfunction


typedef enum {
    Resetting,
    Free,
    Adding,
    Checking,
    Copying
} SummaryState deriving (Bits, Eq, FShow);

(* synthesize *)
module mkSummary(Summary);
    let checkOrAdd <- mkUniqueWrapper3(checkOrAddToChunk);

    let cfg = BRAM_Configure {
        memorySize: valueOf(NumBloomChunks),
        latency: 1,
        outFIFODepth: 3,
        loadFormat: None,
        allowWriteResponseBypass: False
    };
    BRAM2Port#(BloomChunkIndex, BloomChunkParts) bloom <- mkBRAM2Server(cfg);

    Reg#(SummaryState) state <- mkReg(Resetting);
    Reg#(Transaction) txn <- mkRegU;
    Reg#(Timestamp) cycle <- mkReg(0);
    
    rule incrementCycle;
        cycle <= cycle + 1;
    endrule

    // All operations use the same means of traversal
    // They go through each chunk, one by one
    Reg#(Maybe#(BloomChunkIndex)) mReqChunk <- mkReg(Invalid);
    rule requestChunks if (mReqChunk matches tagged Valid .reqChunk);
        bloom.portA.request.put(BRAMRequest {
            write: False,
            responseOnWrite: False,
            address: reqChunk,
            datain: ?
        });
        $fdisplay(stderr, "[%0d] requestChunks: state=", 
            cycle, fshow(state), ", chunk=", fshow(reqChunk));

        mReqChunk <= reqChunk < fromInteger(valueOf(NumBloomChunks)-1)
                     ? Valid(reqChunk+1)
                     : Invalid;
    endrule

    // Similar counter for going through the responses.
    // If Invalid and not Free, it means we're done with the operation.
    Reg#(Maybe#(BloomChunkIndex)) mRespChunk <- mkReg(Valid(0));

    rule doReset if (mRespChunk matches tagged Valid .respChunk &&& state == Resetting);
        bloom.portB.request.put(BRAMRequest {
            write: True,
            responseOnWrite: False,
            address: respChunk,
            datain: unpack(0)
        });
        mRespChunk <= respChunk < fromInteger(valueOf(NumBloomChunks)-1)
                      ? Valid(respChunk+1)
                      : Invalid;
    endrule

    rule resetDone if (mRespChunk matches tagged Invalid &&& state == Resetting);
        state <= Free;
    endrule

    // For add operation, we read and then write the modifications
    rule doAdd_resp if (mRespChunk matches tagged Valid .respChunk &&& state == Adding);
        let data <- bloom.portA.response.get;
        let wrapRes <- checkOrAdd.func(data, respChunk, txn);
        let newData = tpl_2(wrapRes);
        $fdisplay(stderr, "[%0d] doAdd_resp: chunk=", cycle, fshow(respChunk), ", newData=", fshow(newData));

        bloom.portB.request.put(BRAMRequest {
            write: True,
            responseOnWrite: False,
            address: respChunk,
            datain: newData
        });
        mRespChunk <= respChunk < fromInteger(valueOf(NumBloomChunks)-1)
                      ? Valid(respChunk+1)
                      : Invalid;
    endrule

    rule addIsDone if (mRespChunk matches tagged Invalid &&& state == Adding);
        state <= Free;
    endrule

    // For check operation, we read and update compat register
    Reg#(Bool) compat <- mkRegU;
    rule doCheck_resp if (mRespChunk matches tagged Valid .respChunk &&& state == Checking);
        let data <- bloom.portA.response.get;
        let wrapRes <- checkOrAdd.func(data, respChunk, txn);
        let chunkCompat = tpl_1(wrapRes);
        compat <= compat && chunkCompat;
        $fdisplay(stderr, "[%0d] doCheck_resp: chunk=", cycle, fshow(respChunk), ", chunkCompat=", fshow(chunkCompat), ", overall=", fshow(compat && chunkCompat));
        mRespChunk <= respChunk < fromInteger(valueOf(NumBloomChunks)-1)
                      ? Valid(respChunk+1)
                      : Invalid;
    endrule

    rule copyIsDone if (!isValid(mReqChunk) && !isValid(mRespChunk) && state == Copying);
        state <= Free;
    endrule

    interface Put txns;
        method Action put(Transaction t) if (state == Free);
            txn <= t;
            state <= Adding;
            mReqChunk <= Valid(0);
            mRespChunk <= Valid(0);
            compat <= ?;
            $fdisplay(stderr, "[%0d] txns.put: Starting Add operation, txnId=", 
                cycle, fshow(t.txnId));
        endmethod
    endinterface

    interface Server checks;
        interface Put request;
            method Action put(Transaction t) if (state == Free);
                txn <= t;
                state <= Checking;
                mReqChunk <= Valid(0);
                mRespChunk <= Valid(0);
                compat <= True;
                $fdisplay(stderr, "[%0d] checks.request.put: Starting Check operation, txnId=", 
                    cycle, fshow(t.txnId));
            endmethod
        endinterface

        interface Get response;
            method ActionValue#(Bool) get if (mRespChunk matches tagged Invalid &&& state == Checking);
                $fdisplay(stderr, "[%0d] checks.response.get: Completing Check operation, compat=", 
                    cycle, fshow(compat));
                txn <= ?;
                state <= Free;
                mReqChunk <= Invalid;
                mRespChunk <= Invalid;
                let res = compat;
                compat <= ?;
                return res;
            endmethod
        endinterface
    endinterface

    method Action startClearCopy if (state == Free);
        txn <= ?;
        state <= Copying;
        mReqChunk <= Valid(0); // use for reading
        mRespChunk <= Valid(0); // use for writing
        compat <= True;
        $fdisplay(stderr, "[%0d] startClearCopy: Starting Copy operation", cycle);
    endmethod

    interface Get getChunk;
        method ActionValue#(BloomChunkParts) get if (state == Copying);
            let data <- bloom.portA.response.get;
            $fdisplay(stderr, "[%0d] getChunk.get: Got chunk data", cycle);
            return data;
        endmethod
    endinterface

    interface Put setChunk;
        method Action put(BloomChunkParts data) if (mRespChunk matches tagged Valid .respChunk &&& state == Copying);
            $fdisplay(stderr, "[%0d] setChunk.put: Setting chunk ", cycle, fshow(respChunk));
            bloom.portB.request.put(BRAMRequest {
                write: True,
                responseOnWrite: False,
                address: respChunk,
                datain: data
            });
            mRespChunk <= respChunk < fromInteger(valueOf(NumBloomChunks)-1)
                          ? Valid(respChunk+1)
                          : Invalid;
        endmethod
    endinterface

    method Bool isFree = state == Free;
endmodule
