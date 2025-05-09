import BasicTypes::*;
import MainTypes::*;
import Vector::*;
import Summary::*;
import StmtFSM::*;
import GetPut::*;
import ClientServer::*;

module mkSummaryTest(Empty);
    let summary <- mkSummary;

    Reg#(Bit#(16)) testCount <- mkReg(0);
    
    // Flag to switch between behaviors
    Bool useSeparateReadWrite = False; // Change to True when implementing separate read-write handling
    
    // Helper function to check result against expected outcome
    function Action checkResult(Bool expected, String testName);
        return action
            let compat <- summary.checks.response.get;
            $display("Test %0d: ", testCount, testName, " - Expected ", fshow(expected), ", got ", fshow(compat));
            
            if (compat != expected) begin
                $display("TEST FAILED: Expected ", fshow(expected), ", got ", fshow(compat), " for test %0d: ", testCount, testName);
                $finish;
            end
            testCount <= testCount + 1;
        endaction;
    endfunction
    
    // Helper function to create a transaction
    function Transaction makeTxn(Vector#(MaxTxnReadObjs, ObjectId) rObjs, TCount#(MaxTxnReadObjs) numR, 
                               Vector#(MaxTxnWriteObjs, ObjectId) wObjs, TCount#(MaxTxnWriteObjs) numW);
        return Transaction {
            txnId: zeroExtend(pack(testCount)), 
            auxData: 0,
            numReadObjs: numR,
            readObjs: rObjs,
            numWriteObjs: numW,
            writeObjs: wObjs
        };
    endfunction
    
    // Helper to add a transaction to the summary
    function Action addTransaction(Vector#(MaxTxnReadObjs, ObjectId) rObjs, TCount#(MaxTxnReadObjs) numR, 
                                 Vector#(MaxTxnWriteObjs, ObjectId) wObjs, TCount#(MaxTxnWriteObjs) numW,
                                 String desc);
        return action
            let txn = makeTxn(rObjs, numR, wObjs, numW);
            $display("Adding transaction %0d: ", testCount, desc, " with reads = ", fshow(rObjs), 
                     " writes = ", fshow(wObjs));
            summary.txns.put(txn);
        endaction;
    endfunction
    
    // Helper to check compatibility of a transaction
    function Action checkTransaction(Vector#(MaxTxnReadObjs, ObjectId) rObjs, TCount#(MaxTxnReadObjs) numR, 
                                   Vector#(MaxTxnWriteObjs, ObjectId) wObjs, TCount#(MaxTxnWriteObjs) numW,
                                   Bool shouldBeCompatible, String desc);
        return action
            let txn = makeTxn(rObjs, numR, wObjs, numW);
            $display("Checking transaction %0d: ", testCount, desc, " with reads = ", fshow(rObjs), 
                     " writes = ", fshow(wObjs));
            summary.checks.request.put(txn);
        endaction;
    endfunction
    
    let fsm <- mkAutoFSM(seq
        // Basic setup - add initial transactions
        
        // CASE 1: Add transaction with reads = {100, 101}, writes = {200}
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 100;
            readObjs[1] = 101;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 200;
            
            addTransaction(readObjs, 2, writeObjs, 1, "Initial transaction");
        endaction
        
        // TEST GROUP 1: Read-Read compatibility tests
        
        // CASE 2: Read-Read overlap: different behavior based on mode
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 100; // Overlaps with read from Case 1
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            
            checkTransaction(readObjs, 1, writeObjs, 0, useSeparateReadWrite ? True : False, "Read-Read overlap");
        endaction
        checkResult(useSeparateReadWrite ? True : False, "Read-Read overlap");
        
        // CASE 3: Read-Read with different objects: should always be compatible
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 102; // New object, no overlap
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            
            checkTransaction(readObjs, 1, writeObjs, 0, True, "Read-Read different objects");
        endaction
        checkResult(True, "Read-Read different objects");
        
        // TEST GROUP 2: Read-Write conflict tests
        
        // CASE 4: Read-Write conflict (read existing write)
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 200; // Tries to read object that is written by Case 1
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            
            checkTransaction(readObjs, 1, writeObjs, 0, False, "Read-Write conflict (read existing write)");
        endaction
        checkResult(False, "Read-Write conflict (read existing write)");
        
        // CASE 5: Read-Write conflict (write existing read)
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 101; // Tries to write object that is read by Case 1
            
            checkTransaction(readObjs, 0, writeObjs, 1, False, "Read-Write conflict (write existing read)");
        endaction
        // This should be False in both modes, since writing to an object that is being read
        // is a conflict in either implementation
        checkResult(False, "Read-Write conflict (write existing read)");
        
        // TEST GROUP 3: Write-Write conflict tests
        
        // CASE 6: Write-Write conflict
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 200; // Tries to write object that is written by Case 1
            
            checkTransaction(readObjs, 0, writeObjs, 1, False, "Write-Write conflict");
        endaction
        checkResult(False, "Write-Write conflict");
        
        // CASE 7: Write to different object: should be compatible in both modes
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 201; // New object, no overlap
            
            checkTransaction(readObjs, 0, writeObjs, 1, True, "Write to different object");
        endaction
        checkResult(True, "Write to different object");
        
        // Add more complex transaction for mixed scenarios
        
        // CASE 8: Add a transaction with multiple reads and writes
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 300;
            readObjs[1] = 301;
            readObjs[2] = 302;
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 400;
            writeObjs[1] = 401;
            
            addTransaction(readObjs, 3, writeObjs, 2, "Complex transaction");
        endaction
        
        // TEST GROUP 4: Complex scenarios
        
        // CASE 9: Mixed scenario - read existing read, write new object
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 301; // Read from Case 8
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 500; // New write
            
            checkTransaction(readObjs, 1, writeObjs, 1, useSeparateReadWrite ? True : False, "Read existing read, write new object");
        endaction
        checkResult(useSeparateReadWrite ? True : False, "Read existing read, write new object");
        
        // CASE 10: Mixed scenario - read one existing read, write one existing write
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 302; // Read from Case 8
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 400; // Write from Case 8
            
            checkTransaction(readObjs, 1, writeObjs, 1, False, "Read existing read, write existing write");
        endaction
        checkResult(False, "Read existing read, write existing write");
        
        // CASE 11: Mixed scenario - read new object, write existing read
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            readObjs[0] = 303; // New read
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            writeObjs[0] = 300; // Read from Case 8
            
            checkTransaction(readObjs, 1, writeObjs, 1, False, "Read new object, write existing read");
        endaction
        checkResult(False, "Read new object, write existing read");
        
        // TEST GROUP 5: Edge cases
        
        // CASE 12: Empty transaction - should always be compatible
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            
            checkTransaction(readObjs, 0, writeObjs, 0, True, "Empty transaction");
        endaction
        checkResult(True, "Empty transaction");
        
        // CASE 13: Max objects transaction - testing boundary
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            for (Integer i = 0; i < valueOf(MaxTxnReadObjs); i = i + 1) begin
                readObjs[i] = fromInteger(1000 + i);
            end
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            for (Integer i = 0; i < valueOf(MaxTxnWriteObjs); i = i + 1) begin
                writeObjs[i] = fromInteger(2000 + i);
            end
            
            addTransaction(readObjs, fromInteger(valueOf(MaxTxnReadObjs)), 
                          writeObjs, fromInteger(valueOf(MaxTxnWriteObjs)), 
                          "Max objects transaction");
        endaction
        
        // CASE 14: Test with same max transaction - should conflict in both modes
        action
            Vector#(MaxTxnReadObjs, ObjectId) readObjs = ?;
            for (Integer i = 0; i < valueOf(MaxTxnReadObjs); i = i + 1) begin
                readObjs[i] = fromInteger(1000 + i);
            end
            
            Vector#(MaxTxnWriteObjs, ObjectId) writeObjs = ?;
            for (Integer i = 0; i < valueOf(MaxTxnWriteObjs); i = i + 1) begin
                writeObjs[i] = fromInteger(2000 + i);
            end
            
            checkTransaction(readObjs, fromInteger(valueOf(MaxTxnReadObjs)), 
                            writeObjs, fromInteger(valueOf(MaxTxnWriteObjs)), 
                            False, "Same max objects transaction");
        endaction
        checkResult(False, "Same max objects transaction");
        
        // Final display
        action
            $display("All %0d tests passed!", testCount);
        endaction
        
    endseq);
endmodule
