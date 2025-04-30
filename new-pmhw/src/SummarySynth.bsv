import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import Summary::*;
import LFSR::*;

interface SummarySynth;
    method ActionValue#(Bool) isCompatible;
    method Action clear;
    method Action addTxn;

    method Bit#(1) getReadBloom(TIndex#(NumBloomParts) part, TIndex#(BloomPartSize) index);
    method Bit#(1) getWriteBloom(TIndex#(NumBloomParts) part, TIndex#(BloomPartSize) index);
endinterface


(* synthesize *)
module mkSummarySynth(SummarySynth);
    let summary <- mkSummary;

    Bit#(1024) seed = 1024'h8004080008000400000200040001001000010000100020004000000100001000000040000400000010000000400000010000000400000010000000400000010000000400000010000000400000010000000400000010000000400000010000000400000010000000401;
    LFSR#(Bit#(1024)) rng <- mkFeedLFSR(seed);

    Reg#(Bit#(20)) cnt <- mkReg(0);

    rule flipCnt;
        cnt <= cnt+1;
    endrule

    method ActionValue#(Bool) isCompatible if (cnt > 0 && cnt % 2 == 0);
        Transaction txn = unpack(truncate(rng.value));
        rng.next;
        return summary.isCompatible(txn);
    endmethod

    method Action addTxn if (cnt > 0 && cnt % 2 == 1);
        Transaction txn = unpack(truncate(rng.value));
        rng.next;

        summary.addTxn(txn);
    endmethod

    method Action clear if (cnt == 0);
        summary.clear;
    endmethod

    method Bit#(1) getReadBloom(TIndex#(NumBloomParts) part, TIndex#(BloomPartSize) index) = summary.getReadBloom[part][index];
    method Bit#(1) getWriteBloom(TIndex#(NumBloomParts) part, TIndex#(BloomPartSize) index) = summary.getWriteBloom[part][index];
endmodule


