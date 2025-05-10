import BasicTypes::*;
import MainTypes::*;
import Puppetmaster::*;
import TxnDriver::*;
import Executor::*;

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
    method Action systemReset();
    /*
    Get current configuration values.
    */
    method Action fetchConfig();
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
        TCount#(MaxTxnReadObjs) readObjsCount,
        ObjectId readObj0,
        ObjectId readObj1,
        ObjectId readObj2,
        ObjectId readObj3,
        ObjectId readObj4,
        ObjectId readObj5,
        ObjectId readObj6,
        ObjectId readObj7,
        TCount#(MaxTxnWriteObjs) writeObjsCount,
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
    method Action triggerDriver();
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

