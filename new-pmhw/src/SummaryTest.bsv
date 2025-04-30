import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import Summary::*;
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
