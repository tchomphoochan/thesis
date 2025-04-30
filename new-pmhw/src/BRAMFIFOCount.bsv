import FIFO::*;
import BRAMFIFO::*;
import FIFOLevel::*;

module mkBRAMFIFOCount(FIFOCountIfc#(elemType, fifoDepth))
    provisos (Bits(elemType),Bits(fifoDepth), Add#(3, _, fifoDepth));

    FIFO#(elemType) fifo <- mkSizedBRAMFIFO(valueOf(fifoDepth));
    Reg#(UInt#(TLog#(TAdd#(fifoDepth,1)))) cnt <- mkReg(0);

    method Action enq(elemType elem);
        fifo.enq(elem);
        cnt <= cnt+1;
    endmethod

    method Action deq;
        fifo.deq;
        cnt <= cnt-1;
    endmethod

    method elemType first = fifo.first;

    method Bool notFull = cnt < fromInteger(valueOf(fifoDepth));
    method Bool notEmpty = cnt > 0;
    method UInt#(TLog#(TAdd#(fifoDepth,1))) count = cnt;

    method Action clear;
        fifo.clear;
        cnt <= 0;
    endmethod

endmodule
