import BasicTypes::*;
import MainTypes::*;
import Puppetmaster::*;
import TxnDriver::*;
import Executor::*;
import Vector::*;
import GetPut::*;
import Connectable::*;
import ClientServer::*;
import Clocks::*;

/*
Top-level configuration for testing
*/
typedef struct {
    Bool useSimulatedTxnDriver;
    Bool useSimulatedPuppets;
    Bit#(16) numPuppets;
} TopConfig deriving (Bits, Eq, FShow);

/*
Connectal software-to-hardware interface.
*/
interface S2HMessage;
    /*
    Force reset everything
    */
    method Action systemReset;
    /*
    Get current configuration values.
    */
    method Action fetchConfig;
    /*
    Set configuration values.
    */
    method Action setConfig(TopConfig cfg);
    /*
    Add transaction to Puppetmaster.
    */
    method Action addTransaction(
        TransactionId txnId,
        AuxData auxData,
        TCount#(MaxTxnReadObjs) numReadObjs,
        ObjectId readObj0,
        ObjectId readObj1,
        ObjectId readObj2,
        ObjectId readObj3,
        ObjectId readObj4,
        ObjectId readObj5,
        ObjectId readObj6,
        ObjectId readObj7,
        TCount#(MaxTxnWriteObjs) numWriteObjs,
        ObjectId writeObj0,
        ObjectId writeObj1,
        ObjectId writeObj2,
        ObjectId writeObj3,
        ObjectId writeObj4,
        ObjectId writeObj5,
        ObjectId writeObj6,
        ObjectId writeObj7
    );
    /*
    For fake transactions, a trigger is needed to send transactions to Puppetmaster.
    */
    method Action triggerDriver;
    /*
    For host to report that it has completed a transaction
    */
    method Action reportWorkDone(TransactionId txnId);
endinterface

/*
Connectal hardware-to-software interface.
*/
interface H2SMessage;
    method Action configData(TopConfig cfg);
    method Action transactionScheduled(TransactionId txnId);
endinterface

/*
Connectal automatically sets up software-to-host communication based on this interface.
We simply need to implement those interfaces so the hardware knows what to do with these messages.
*/
interface Connectal;
    interface S2HMessage s2h;
endinterface

module mkConnectal#(
    /*
    Connectal automatically sets up host-to-software communication based on this interface.
    Hardware can call these "indication" methods to send messages.
    */
    H2SMessage h2s
)(Connectal);
    Reset curRst <- exposeCurrentReset;
    Clock curClk <- exposeCurrentClock;

    PulseWire rstIn <- mkPulseWire;
    MakeResetIfc newRstGen <- mkReset(2, rstIn, curClk);
    Reset newRstAsync = newRstGen.new_rst;
    Reset newRst <- mkSyncReset(2, newRstAsync, curClk);
    Reset sysRst <- mkResetEither(curRst, newRst);

    // Input side
    RealTxnDriver realTxnDriver <- mkRealTxnDriver(reset_by sysRst);
    FakeTxnDriver fakeTxnDriver <- mkFakeTxnDriver(reset_by sysRst);
    TxnDriver allTxnDrivers[2] = {
        realTxnDriver.txnDriver,
        fakeTxnDriver.txnDriver
    };
    TxnDriverMux#(2) txnDriverMux <- mkTxnDriverMux(arrayToVector(allTxnDrivers));

    // Processing component
    // Create the actual Puppetmaster instance.
    Puppetmaster pm <- mkPuppetmaster(reset_by sysRst);

    // Output side
    RealExecutor realExecutor <- mkRealExecutor(reset_by sysRst);
    FakeExecutor fakeExecutor <- mkFakeExecutor(reset_by sysRst);
    Executor allExecutors[2] = {
        realExecutor.executor,
        fakeExecutor.executor
    };
    ExecutorMux#(2) executorMux <- mkExecutorMux(arrayToVector(allExecutors));

    /*
    Internal connections
    */

    // Input side to processing component
    mkConnection(txnDriverMux.txnDriver.transactions, pm.transactions);

    // Processing component to output side
    mkConnection(pm.scheduled, executorMux.executor.request);

    // Output side back to processing component (free the completed transactions)
    mkConnection(executorMux.executor.response, toPut(pm.workDone));

    // Real executor controller pipes result back to host
    // so host can do the work
    mkConnection(realExecutor.toHost, toPut(h2s.transactionScheduled));

    /*
    External interfaces
    */
    interface S2HMessage s2h;
        method Action systemReset();
            rstIn.send();
        endmethod

        method Action fetchConfig();
            // TODO
        endmethod

        method Action setConfig(TopConfig cfg);
            // TODO
        endmethod

        method Action addTransaction(
            TransactionId txnId,
            AuxData auxData,
            TCount#(MaxTxnReadObjs) numReadObjs,
            ObjectId readObj0,
            ObjectId readObj1,
            ObjectId readObj2,
            ObjectId readObj3,
            ObjectId readObj4,
            ObjectId readObj5,
            ObjectId readObj6,
            ObjectId readObj7,
            TCount#(MaxTxnWriteObjs) numWriteObjs,
            ObjectId writeObj0,
            ObjectId writeObj1,
            ObjectId writeObj2,
            ObjectId writeObj3,
            ObjectId writeObj4,
            ObjectId writeObj5,
            ObjectId writeObj6,
            ObjectId writeObj7
        );
            ObjectId readObjs[8] = {
                readObj0,
                readObj1,
                readObj2,
                readObj3,
                readObj4,
                readObj5,
                readObj6,
                readObj7
            };
            ObjectId writeObjs[8] = {
                writeObj0,
                writeObj1,
                writeObj2,
                writeObj3,
                writeObj4,
                writeObj5,
                writeObj6,
                writeObj7
            };
            let txn = Transaction {
                txnId: txnId,
                auxData: auxData,
                numReadObjs: numReadObjs,
                readObjs: arrayToVector(readObjs),
                numWriteObjs: numWriteObjs,
                writeObjs: arrayToVector(writeObjs)
            };

            if (txnDriverMux.selected == 0) begin
                realTxnDriver.fromHost.put(txn);
            end else if (txnDriverMux.selected == 1) begin
                fakeTxnDriver.fromHost.put(txn);
            end

        endmethod

        method Action triggerDriver;
            fakeTxnDriver.trigger;
        endmethod

        method Action reportWorkDone(TransactionId txnId);
            realExecutor.fromHost.put(txnId);
        endmethod
    endinterface

endmodule
