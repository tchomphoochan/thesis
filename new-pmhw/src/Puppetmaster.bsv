import BasicTypes::*;
import MainTypes::*;
import GetPut::*;
import FIFO::*;
import SpecialFIFOs::*;
import Vector::*;

import Summary::*;

typedef 128 InputBufferSize;
typedef 128 DoneBufferSize;

/*
Output scheduling message from Puppetmaster
*/
typedef struct {
    TransactionId transactionId;
} ScheduleMessage deriving (Bits, Eq, FShow);

/*
Input work-done message back into Puppetmaster
*/
typedef struct {
    TransactionId transactionId;
} WorkDoneMessage deriving (Bits, Eq, FShow);

/*
Puppetmaster interface
*/
interface Puppetmaster;
    method Action setNumPuppets(Bit#(16) numPuppets);
    method Bit#(16) getNumPuppets() numPuppets);

    interface Put#(Transaction) transactions;
    interface Get#(ScheduleMessage) scheduled;
    interface Put#(WorkDoneMessage) workDone;
endinterface

/*
Steps:
- Drain done
- Rebuild summary:
    - If have element to add to newSummary, add to newSummary.
    - If not, clear currentSummary, and newSummary becomes currentSummary.
        - We know for sure that (new) currentSummary hasn't been modified this cycle yet,
          so the scheduling step could proceed normally.
- Schedule (handle input buffer):
    - Add to currentSummary.
    - No need to add to newSummary yet. That'll be taken care of by the rebuilding loop.
- Receive user input into inputBuffer
*/

(* descending_urgency = "trySchedule, transactions_put" *)
module mkPuppetmaster(Puppetmaster);
    // List of inputs
    FIFO#(Transaction) inputBuffer <- mkSizedBRAMFIFO(InputBufferSize);

    // Actual list of active transactions
    FIFO#(Transaction) activeBuffer <- mkSizedBRAMFIFO(InputBufferSize);

    // List of completed transactions, so we can remove from our summary.
    TransactionId sentinel = maxValue;
    Vector#(DoneBufferSize, Reg#(TransactionId)) doneBuffer <- replicateM(mkReg(sentinel));

    // Bloom filter summary of active transactions
    Vector#(2, Summary) summaries <- replicateM(mkSummary);
    Reg#(Bit#(1)) summaryIndex <- mkReg(0);

    // Output messages
    FIFO#(ScheduleMessage) decisions <- mkFIFO;

    // Cycle through the active transactions to add to the new summary
    // If the transaction was supposed to be removed, then just skip it (and take it out of doneBuffer).
    Reg#(Bit#(TAdd#(1, TLog#(InputBufferSize)))) refreshCnt <- mkReg;
    rule updateNewSummary;
        let txn = activeBuffer.first;
        activeBuffer.deq;
        case findElem(txn.transactionId, doneBuffer) matches
            tagged Valid .idx: begin
                doneBuffer[idx] <= junk;
            end
            tagged Invalid: begin
                let newSummary = summaries[1-summaryIndex];
                newSummary.addTxn(txn);
                refreshCnt <= refreshCnt + 1;
            end
        end
    endrule

    // Clear the current summary and switch over to the new one.
    rule refreshSummary if (refreshCnt == activeBufferCnt);
        summaries[summaryIndex].clear;
        summaryIndex <= 1-summaryIndex;
    endrule

    // Read input from head of input buffer
    // If compatible, schedule. If not, put it back at the tail of the queue.
    rule trySchedule;
        let txn = inputBuffer.first;
        inputBuffer.deq;
        let currentSummary = summaries[summaryIndex];
        if (currentSummary.isCompatible(txn)) begin
            activeBuffer.enq(txn);
            decisions.enq(txn);
            currentSummary.addTxn(txn);
        end else begin
            inputBuffer.enq(txn);
        end
    endrule

    // Puppets 
    Bit#(16) defaultNumPuppets = 8;
    Reg#(Bit#(16)) numPuppets <- mkReg(defaultNumPuppets);

    method Action setNumPuppets(Bit#(16) _numPuppets);
        numPuppets <= _numPuppets;
    endmethod
    method Action getNumPuppets = numPuppets;

    interface transactions = toPut(inputBuffer);

    interface scheduled = toGet(decisions);

    // Mark a transaction as done by adding it to doneBuffer.
    interface Put workDone;
        method Action put(WorkDoneMessage msg);
            let idx = findElem(sentinel, doneBuffer);
            doneBuffer[idx] <= msg.transactionId;
        endmethod
    endinterface

endmodule
