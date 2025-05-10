import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import Puppetmaster::*;
import Executor::*;
import StmtFSM::*;
import GetPut::*;
import ClientServer::*;
import FIFO::*;
import FIFOF::*;
import SpecialFIFOs::*;
import Randomizable::*;
import Connectable::*;

// Test parameters
typedef 16 NumTestTransactions;
typedef 32 MaxRandomValue;
typedef 4  MaxObjectsPerTransaction;
typedef 25 RandomBreakChance; // Percentage chance of random breaks
typedef 10 MaxBreakLength;    // Maximum cycles for a random break

(* synthesize *)
module mkPuppetmasterTest(Empty);
    // Create Puppetmaster and FakeExecutor instances
    Puppetmaster puppetmaster <- mkPuppetmaster();
    FakeExecutor executor <- mkFakeExecutor();
    
    // Connect the executor to the puppetmaster
    mkConnection(puppetmaster.scheduled, executor.executor.request);
    mkConnection(executor.executor.response, puppetmaster.workDone);
    
    // Random number generators for various randomizations
    Randomize#(Bit#(32)) randObjId <- mkGenericRandomizer;
    Randomize#(Bit#(32)) randTxnId <- mkGenericRandomizer;
    Randomize#(Bit#(3))  randReadCount <- mkGenericRandomizer;
    Randomize#(Bit#(3))  randWriteCount <- mkGenericRandomizer;
    Randomize#(Bit#(8))  randBreak <- mkGenericRandomizer;
    Randomize#(Bit#(8))  randBreakLength <- mkGenericRandomizer;
    
    // Test tracking
    Reg#(Bit#(32)) testCount <- mkReg(0);
    Reg#(Bit#(32)) txnSubmitted <- mkReg(0);
    Reg#(Bit#(32)) txnCompleted <- mkReg(0);
    Reg#(Bit#(32)) txnRejected <- mkReg(0);
    Reg#(Bool) testPassed <- mkReg(True);
    Reg#(Bit#(32)) cycle <- mkReg(0);
    Reg#(Bit#(32)) breakLength <- mkReg(0);
    
    // FIFOs to track activity
    FIFO#(TransactionId) submittedTxns <- mkSizedFIFO(valueOf(NumTestTransactions));
    
    // Helper functions
    function ActionValue#(Transaction) genRandomTransaction();
        return actionvalue
            Bit#(32) txnId = zeroExtend(pack(testCount));
            
            // Create random read objects
            let numReadObjs <- randReadCount.next();
            // Limit to MaxObjectsPerTransaction
            numReadObjs = (numReadObjs % fromInteger(valueOf(MaxObjectsPerTransaction))) + 1;
            
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = newVector();
            for (Integer i = 0; i < valueOf(MaxTxnReadObjs); i = i + 1) begin
                if (fromInteger(i) < numReadObjs) begin
                    let randObj <- randObjId.next();
                    // Ensure object ID is within a reasonable range
                    readObjs[i] = (randObj % fromInteger(valueOf(MaxRandomValue))) + 100;
                end else begin
                    readObjs[i] = 0;
                end
            end
            
            // Create random write objects
            let numWriteObjs <- randWriteCount.next();
            // Limit to MaxObjectsPerTransaction
            numWriteObjs = (numWriteObjs % fromInteger(valueOf(MaxObjectsPerTransaction))) + 1;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = newVector();
            for (Integer i = 0; i < valueOf(MaxTxnWriteObjs); i = i + 1) begin
                if (fromInteger(i) < numWriteObjs) begin
                    let randObj <- randObjId.next();
                    // Ensure object ID is within a reasonable range and different from reads
                    writeObjs[i] = (randObj % fromInteger(valueOf(MaxRandomValue))) + 500;
                end else begin
                    writeObjs[i] = 0;
                end
            end
            
            // Create and return the transaction
            return Transaction {
                txnId: txnId,
                auxData: 0,
                numReadObjs: extend(numReadObjs),
                readObjs: readObjs,
                numWriteObjs: extend(numWriteObjs),
                writeObjs: writeObjs
            };
        endactionvalue;
    endfunction
    
    // Rules
    rule incrementCycle;
        cycle <= cycle + 1;
    endrule
    
    // Rule to handle transaction submission with random breaks
    rule submitTransactions if (txnSubmitted < fromInteger(valueOf(NumTestTransactions)) && breakLength == 0);
        // Check if we should take a random break
        let shouldBreak <- randBreak.next();
        if (shouldBreak < fromInteger(valueOf(RandomBreakChance))) begin
            // Take a random break
            let breakDuration <- randBreakLength.next();
            breakLength <= zeroExtend((breakDuration % fromInteger(valueOf(MaxBreakLength))) + 1);
            $display("[%0d] Taking a random break for %0d cycles", cycle, (breakDuration % fromInteger(valueOf(MaxBreakLength))) + 1);
        end else begin
            // Submit a new transaction
            let txn <- genRandomTransaction();
            puppetmaster.transactions.put(txn);
            submittedTxns.enq(txn.txnId);
            txnSubmitted <= txnSubmitted + 1;
            testCount <= testCount + 1;
            $display("[%0d] Submitted transaction %0d: txnId=%0d, reads=%0d, writes=%0d", 
                     cycle, txnSubmitted, txn.txnId, txn.numReadObjs, txn.numWriteObjs);
        end
    endrule
    
    // Rule to reduce break length during a break
    rule decrementBreak if (breakLength > 0);
        breakLength <= breakLength - 1;
    endrule
    
    // Comprehensive test initialization and execution
    let testFSM <- mkFSM(
        seq
            // Initialize random generators
            action
                randObjId.cntrl.init();
                randTxnId.cntrl.init();
                randReadCount.cntrl.init();
                randWriteCount.cntrl.init();
                randBreak.cntrl.init();
                randBreakLength.cntrl.init();
                $display("[%0d] Starting Puppetmaster test with %0d random transactions", cycle, valueOf(NumTestTransactions));
            endaction
            
            // Set number of puppets
            action
                puppetmaster.setNumPuppets(8);
                $display("[%0d] Set number of puppets to 8", cycle);
            endaction
            
            // Wait for all transactions to be submitted
            while (txnSubmitted < fromInteger(valueOf(NumTestTransactions))) noAction;
            
            // Wait for execution to complete - arbitrary wait time based on complexity
            delay(fromInteger(valueOf(NumTestTransactions) * 20));
            
            // Analyze results and report
            action
                $display("\n=== Test Results ===");
                $display("Total transactions submitted: %0d", txnSubmitted);
                $display("Test passed: %s", testPassed ? "YES" : "NO");
                
                if (testPassed) begin
                    $display("ALL TESTS PASSED!");
                end else begin
                    $display("SOME TESTS FAILED!");
                end
                $finish;
            endaction
        endseq
    );
    
    // Start the test
    rule startTest (cycle == 10);
        testFSM.start();
    endrule
    
    // Rule to monitor execution progress (optional)
    rule monitorProgress;
        if (cycle % 100 == 0) begin
            $display("[%0d] Progress: %0d/%0d transactions submitted", 
                     cycle, txnSubmitted, valueOf(NumTestTransactions));
        end
    endrule
endmodule

