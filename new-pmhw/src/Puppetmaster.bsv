import BasicTypes::*;
import MainTypes::*;
import GetPut::*;
import FIFO::*;
import FIFOF::*;
import SpecialFIFOs::*;
import ClientServer::*;
import Vector::*;

import Summary::*;

typedef 4 InputBufferSize;
typedef 4 LookaheadBufferSize;
typedef 16 ActiveBufferSize; // maximum number of concurrently scheduled transactions
typedef 512 RefreshDuration;

/*
Output scheduling message from Puppetmaster
*/
typedef struct {
    TransactionId txnId;
} ScheduleMessage deriving (Bits, Eq, FShow);

/*
Input work-done message back into Puppetmaster
*/
typedef struct {
    TransactionId txnId;
} WorkDoneMessage deriving (Bits, Eq, FShow);

/*
Puppetmaster interface
*/
interface Puppetmaster;
    method Action setNumPuppets(Bit#(16) numPuppets);
    method Bit#(16) getNumPuppets;

    interface Put#(Transaction) transactions;
    interface Get#(ScheduleMessage) scheduled;
    interface Put#(WorkDoneMessage) workDone;
endinterface

typedef enum {
    Normal, // Normal operation
    StartSwitch, // Paused everything, waiting for shadow queue to be drained to start the copying process
    Switching // Copying
} PuppetmasterState deriving (Bits, Eq, FShow);

(* descending_urgency = "trySchedule, transactions_put" *)
module mkPuppetmaster(Puppetmaster);
    Reg#(PuppetmasterState) state <- mkReg(Normal);

    FIFO#(Transaction) inputQ <- mkSizedFIFO(valueOf(InputBufferSize));
    FIFOF#(Transaction) lookaheadReinsert <- mkPipelineFIFOF;
    FIFO#(Transaction) lookaheadBuffer <- mkSizedFIFO(valueOf(LookaheadBufferSize));

    // Arbitrate inputs into the lookahead buffer
    rule drainInput if (state == Normal);
        // Always prioritize re-inserts first
        if (lookaheadReinsert.notEmpty) begin
            let txn = lookaheadReinsert.first;
            lookaheadReinsert.deq;
            lookaheadBuffer.enq(txn);
        end else begin
            let txn = inputQ.first;
            inputQ.deq;
            lookaheadBuffer.enq(txn);
        end
    endrule

    Summary mainSummary <- mkSummary;
    FIFOF#(Transaction) shadowQ <- mkFIFOF;
    FIFO#(ScheduleMessage) schedQ <- mkSizedFIFO(valueOf(ActiveBufferSize));

    // First step for handling a transaction: ask tok check with the summary
    rule lookaheadCheckConflict if (state == Normal);
        let txn = lookaheadBuffer.first;
        mainSummary.checks.request.put(txn);
    endrule

    Vector#(ActiveBufferSize, Reg#(Maybe#(Transaction))) activeTxns <- replicateM(mkReg(Invalid));
    Reg#(TIndex#(ActiveBufferSize)) activeTxnsHead <- mkReg(0);
    Reg#(TIndex#(ActiveBufferSize)) activeTxnsTail <- mkReg(0);

    Bool activeTxnsEmpty = activeTxnsHead == activeTxnsTail;
    Bool activeTxnsFull = activeTxnsTail+1 == activeTxnsHead;

    // Then, depending on the summary result, decide whether to re-insert or proceed to schedule
    rule lookaheadResult if (state == Normal && !activeTxnsFull);
        let txn = lookaheadBuffer.first;
        lookaheadBuffer.deq;
        let isCompat <- mainSummary.checks.response.get;
        if (isCompat) begin
            // if compat, tell user to run the txn (schedule)
            schedQ.enq(ScheduleMessage { txnId: txn.txnId });
            // add to current summary
            mainSummary.txns.put(txn);
            // also add to shadow queue to be added to shadow filter
            shadowQ.enq(txn);
            // add to active list (so next refresh cycle we know to include this transaction)
            activeTxns[activeTxnsTail] <= Valid(txn);
            activeTxnsTail <= activeTxnsTail + 1;
        end else begin
            // if not compat, put it back into the lookahead buffer
            lookaheadReinsert.enq(txn);
        end
    endrule

    // Invariant: for a given txn in activeTxns, txn is either reflected in shadowSummary or is still in shadowQ.
    Summary shadowSummary <- mkSummary;

    Reg#(TIndex#(ActiveBufferSize)) shadowPtr <- mkReg(0);
    Reg#(Bool) shadowComplete <- mkReg(True);
    Reg#(TIndex#(RefreshDuration)) refreshTimer <- mkReg(fromInteger(valueOf(RefreshDuration)-1));

    FIFOF#(WorkDoneMessage) workDoneQ <- mkSizedFIFOF(valueOf(ActiveBufferSize));

    // Go through all the active txns and put them in Bloom filter
    rule shadowUpdate;
        // Prioritize new txns first
        // Otherwise, go through the active list (from tail to head)
        if (shadowQ.notEmpty) begin
            // Handle the queue
            let txn = shadowQ.first;
            shadowQ.deq;
            shadowSummary.txns.put(txn);
        end else if (!shadowComplete) begin

            case (activeTxns[shadowPtr]) matches
                // Skip invalid ones
                tagged Invalid: begin
                    // nothing to do
                end
                // For valid ones, check against workDoneQ
                tagged Valid .txn: begin
                    Bool skip = False;
                    if (workDoneQ.notEmpty) begin
                        let doneTxnId = workDoneQ.first.txnId;
                        workDoneQ.deq;
                        if (doneTxnId == txn.txnId) begin
                            // We found a match with the queue of done transactions
                            // so we skip adding this to the Bloom filter
                            skip = True;
                            // We also mark it invalid
                            activeTxns[shadowPtr] <= Invalid;
                        end
                    end
                    // Otherwise, just add to the filter
                    if (!skip) begin
                        shadowSummary.txns.put(txn);
                    end
                end
            endcase

            // "Increment" counter (we go from tail to head)
            shadowPtr <= shadowPtr-1;
            if (shadowPtr == activeTxnsHead) begin
                shadowComplete <= True;
            end

        end
    endrule

    // Tick refresh timer
    rule tickRefreshTimer if (state == Normal);
        if (refreshTimer > 0) begin
            refreshTimer <= refreshTimer-1;
        end else begin
            state <= StartSwitch;
        end
    endrule

    // wait for shadowQ to be drained and all active txns to be handled
    rule performSwitch if (state == StartSwitch && !shadowQ.notEmpty && shadowComplete);
        shadowSummary.startClearCopy;
        mainSummary.startClearCopy;
        state <= Switching;
    endrule

    rule clearShadow if (state == Switching);
        shadowSummary.setChunk.put(unpack(0));
    endrule
    rule copyShadowToMain if (state == Switching);
        let data <- shadowSummary.getChunk.get;
        mainSummary.setChunk.put(data);
    endrule
    rule drainMain if (state == Switching);
        let _ <- mainSummary.getChunk.get;
    endrule

    // When the switch is done (main summary is now active), reinitialize stuff
    rule switchDone if (state == Switching && mainSummary.isFree && shadowSummary.isFree);
        if (activeTxnsEmpty) begin
            shadowPtr <= ?;
            shadowComplete <= activeTxnsEmpty;
        end else begin
            shadowPtr <= activeTxnsTail-1;
            shadowComplete <= False;
        end
        refreshTimer <= fromInteger(valueOf(RefreshDuration)-1);
        state <= Normal;
    endrule

    // Puppets 
    Bit#(16) defaultNumPuppets = 8;
    Reg#(Bit#(16)) numPuppets <- mkReg(defaultNumPuppets);

    method Action setNumPuppets(Bit#(16) _numPuppets);
        numPuppets <= _numPuppets;
    endmethod
    method Bit#(16) getNumPuppets = numPuppets;

    interface transactions = toPut(inputQ);
    interface scheduled = toGet(schedQ);
    interface workDone = toPut(workDoneQ);

endmodule
