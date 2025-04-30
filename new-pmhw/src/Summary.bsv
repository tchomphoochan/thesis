import BasicTypes::*;
import MainTypes::*;
import Vector::*;

typedef Vector#(NumBloomParts, Bit#(BloomPartSize)) BloomFilter;

interface Summary;
    method Bool isCompatible(Transaction txn);
    method Action clear;
    method Action addTxn(Transaction txn);

    method BloomFilter getReadBloom;
    method BloomFilter getWriteBloom;
endinterface

module mkSummary(Summary);
    Reg#(BloomFilter) readBloom <- mkReg(defaultValue);
    Reg#(BloomFilter) writeBloom <- mkReg(defaultValue);

    // Helper function for figuring out whether there's a conflict between
    // a set of objects and a Bloom filter
    function Bool genericCompat(BloomFilter bloom, TNum#(max_n) numObjs, Vector#(max_n, ObjectHash) objs);
        Bool compatible = True;
        for (Integer objIdx = 0; objIdx < valueOf(max_n); objIdx = objIdx+1) begin
            if (fromInteger(objIdx) < numObjs) begin
                Bool allPartsMatch = True;
                let objHash = objs[objIdx];
                for (Integer part = 0; part < valueOf(NumBloomParts); part = part+1) begin
                    let index = objHash[part];
                    if (bloom[part][index] != 1'b1) allPartsMatch = False;
                end
                // If so, we have a conflict.
                if (allPartsMatch) compatible = False;
            end
        end
        return compatible;
    endfunction
 
    method Bool isCompatible(Transaction txn);
        return genericCompat(readBloom, txn.numWriteObjs, txn.writeObjs)
            && genericCompat(writeBloom, txn.numReadObjs, txn.readObjs)
            && genericCompat(writeBloom, txn.numWriteObjs, txn.writeObjs);
    endmethod

    method Action clear;
        readBloom <= unpack(0);
        writeBloom <= unpack(0);
    endmethod

    method Action addTxn(Transaction txn);
        BloomFilter newReadBloom = readBloom;
        BloomFilter newWriteBloom = writeBloom;

        for (Integer part = 0; part < valueOf(NumBloomParts); part = part+1) begin
            for (Integer objIdx = 0; objIdx < maxTxnReadObjs; objIdx = objIdx+1) begin
                if (fromInteger(objIdx) < txn.numReadObjs) begin
                    newReadBloom[part][txn.readObjs[objIdx][part]] = 1'b1;
                end
            end
        end

        for (Integer part = 0; part < valueOf(NumBloomParts); part = part+1) begin
            for (Integer objIdx = 0; objIdx < maxTxnWriteObjs; objIdx = objIdx+1) begin
                if (fromInteger(objIdx) < txn.numWriteObjs) begin
                    newWriteBloom[part][txn.writeObjs[objIdx][part]] = 1'b1;
                end
            end
        end

        readBloom <= newReadBloom;
        writeBloom <= newWriteBloom;
    endmethod

    method BloomFilter getReadBloom = readBloom;
    method BloomFilter getWriteBloom = writeBloom;
endmodule

