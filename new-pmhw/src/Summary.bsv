import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import List::*;

typedef Vector#(NumBloomParts, Bit#(BloomPartSize)) BloomFilter;

interface Summary;
    method Bool isCompatible(Transaction txn);
    method Action clear;
    method Action addTxn(Transaction txn);

    method BloomFilter getReadBloom;
    method BloomFilter getWriteBloom;
endinterface

(* synthesize *)
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

import StmtFSM::*;

module mkSummaryTest(Empty);
    let summary <- mkSummary;

    let fsm <- mkAutoFSM(seq
        // Add read=0,1, write=2
        action
            Vector#(MaxTxnReadObjs, ObjectHash) readObjs = ?;
            let numReadObjs = 2;
            readObjs[0] = unpack(0);
            readObjs[1] = unpack(1);

            Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs = ?;
            let numWriteObjs = 1;
            writeObjs[0] = unpack(2);

            let txn = Transaction { transactionId: ?, auxData: ?, numReadObjs: numReadObjs, readObjs: readObjs, numWriteObjs: numWriteObjs, writeObjs: writeObjs };
            summary.addTxn(txn);
        endaction

        // action
        //     $display("Read: ", fshow(summary.getReadBloom));
        //     $display("Write:", fshow(summary.getWriteBloom));
        // endaction

        // Read elements overlap with existing read elements: should be fine.
        action
            Vector#(MaxTxnReadObjs, ObjectHash) readObjs = ?;
            let numReadObjs = 1;
            readObjs[0] = unpack(1);

            Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs = ?;
            let numWriteObjs = 0;

            let txn = Transaction { transactionId: ?, auxData: ?, numReadObjs: numReadObjs, readObjs: readObjs, numWriteObjs: numWriteObjs, writeObjs: writeObjs };
            let compat = summary.isCompatible(txn);
            let expected = True;

            if (compat != expected) begin
                $display("Expected ", fshow(expected), ", got", fshow(compat), " for: ", fshow(txn));
                $finish;
            end
        endaction

        // Write elements overlap with existing read elements: should conflict.
        action
            Vector#(MaxTxnReadObjs, ObjectHash) readObjs = ?;
            let numReadObjs = 0;

            Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs = ?;
            let numWriteObjs = 1;
            writeObjs[0] = unpack(0);

            let txn = Transaction { transactionId: ?, auxData: ?, numReadObjs: numReadObjs, readObjs: readObjs, numWriteObjs: numWriteObjs, writeObjs: writeObjs };
            let compat = summary.isCompatible(txn);
            let expected = False;

            if (compat != expected) begin
                $display("Expected ", fshow(expected), ", got ", fshow(compat), " for: ", fshow(txn));
                $finish;
            end
        endaction

        // Read elements overlap with existing write elements: should conflict.
        action
            Vector#(MaxTxnReadObjs, ObjectHash) readObjs = ?;
            let numReadObjs = 1;
            readObjs[0] = unpack(2);

            Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs = ?;
            let numWriteObjs = 0;

            let txn = Transaction { transactionId: ?, auxData: ?, numReadObjs: numReadObjs, readObjs: readObjs, numWriteObjs: numWriteObjs, writeObjs: writeObjs };
            let compat = summary.isCompatible(txn);
            let expected = False;

            if (compat != expected) begin
                $display("Expected ", fshow(expected), ", got ", fshow(compat), " for: ", fshow(txn));
                $finish;
            end
        endaction

        // Read elements overlap with existing read elements, non-overlapping write elements
        action
            Vector#(MaxTxnReadObjs, ObjectHash) readObjs = ?;
            let numReadObjs = 4;
            readObjs[0] = unpack(0);
            readObjs[1] = unpack(1);
            readObjs[2] = unpack(6);
            readObjs[3] = unpack(7);

            Vector#(MaxTxnWriteObjs, ObjectHash) writeObjs = ?;
            let numWriteObjs = 4;
            writeObjs[0] = unpack(8);
            writeObjs[1] = unpack(9);
            writeObjs[2] = unpack(10);
            writeObjs[3] = unpack(11);

            let txn = Transaction { transactionId: ?, auxData: ?, numReadObjs: numReadObjs, readObjs: readObjs, numWriteObjs: numWriteObjs, writeObjs: writeObjs };
            let compat = summary.isCompatible(txn);
            let expected = True;

            if (compat != expected) begin
                $display("Expected ", fshow(expected), ", got ", fshow(compat), " for: ", fshow(txn));
                $finish;
            end
        endaction

    endseq);
endmodule
