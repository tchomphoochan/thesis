import BasicTypes::*;
import MainTypes::*;
import Puppetmaster::*;
import GetPut::*;
import FIFO::*;
import SpecialFIFOs::*;
import Vector::*;

/*
Output side of the Puppetmaster scheduler.
*/
interface Executor;
    // Puppetmaster tells the executor what transaction to run
    interface Put#(ScheduleMessage) requests;

    // The executor lets Puppetmaster know when a transaction is completed.
    interface Get#(WorkDoneMessage) responses;
endinterface


/*
Output MUX
*/
interface ExecutorMux#(numeric type n);
    method TIndex#(n) selected;
    method Action select(TIndex#(n) idx);
    interface Executor executor;
endinterface

module mkExecutorMux(Vector#(n, Executor) execs, ExecutorMux#(n) ifc);
    Reg#(TIndex#(n)) index <- mkRegU;

    method TIndex#(n) selected = index;

    method Action select(TIndex#(n) idx);
        index <= idx;
    endmethod

    interface executor = execs[index];
endmodule


/*
Real executor that relays work message to host CPU to perform actual work.
*/
interface RealExecutor;
    interface Executor executor;
    interface Get#(ScheduleMessage) toHost;
    interface Put#(WorkDoneMessage) fromHost;
endinterface

module mkRealExecutor(RealExecutor);
    FIFO#(ScheduleMessage) reqFF <- mkBypassFIFO;
    FIFO#(WorkDoneMessage) doneFF <- mkBypassFIFO;

    interface toHost = toGet(reqFF);
    interface fromHost = toPut(doneFF);

    interface Executor executor;
        interface requests = toPut(reqFF);
        interface responses = toGet(doneFF);
    endinterface
endmodule


/*
Fake executor that just busy-waits.
*/
interface FakeExecutor;
    interface Executor executor;
endinterface

typedef struct {
    Timestamp startTime;
    TransactionId txnId;
} PuppetData deriving (Bits, Eq, FShow);

typedef 8 MaxSimulatedPuppets;

module mkFakeExecutor(FakeExecutor);
    Timestamp simCycles = 50;

    Reg#(Timestamp) cycle <- mkReg(0);
    Vector#(MaxSimulatedPuppets, Reg#(Maybe#(PuppetData))) puppets <- replicateM(mkReg(Invalid));

    FIFO#(WorkDoneMessage) doneFF <- mkFIFO;

    (* no_implicit_conditions, fire_when_enabled *)
    rule tick;
        cycle <= cycle+1;
    endrule

    function Bool isDone(Maybe#(PuppetData) mPuppet);
        case (mPuppet) matches
            tagged Valid .puppet: begin
                return cycle - puppet.startTime >= simCycles;
            end
            tagged Invalid: begin
                return False;
            end
        endcase
    endfunction

    rule reportDone if (findIndex(isDone, readVReg(puppets)) matches tagged Valid .puppetId);
        let puppet = fromMaybe(?, puppets[puppetId]);
        doneFF.enq(WorkDoneMessage { txnId: puppet.txnId });
        puppets[puppetId] <= Invalid;
        $fdisplay(stderr, "[%0d] FakeExecutor: txn id=", cycle, fshow(puppet.txnId), " finished executing on puppet ", puppetId);
    endrule
    
    interface Executor executor;
        interface Put requests;
            method Action put(ScheduleMessage req) if (findElem(Invalid, readVReg(puppets)) matches tagged Valid .puppetId);
                $fdisplay(stderr, "[%0d] FakeExecutor: starting txn id=", cycle, fshow(req.txnId), " on puppet ", puppetId);
                puppets[puppetId] <= Valid(PuppetData { startTime: cycle, txnId: req.txnId });
            endmethod
        endinterface

        interface responses = toGet(doneFF);
    endinterface

endmodule
