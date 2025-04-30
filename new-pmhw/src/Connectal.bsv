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
        TransactionId transactionId,
        AuxData auxData,
        Bit#(Log#(MaxTxnReadObjs)) readObjsCount,
        ObjectId readObj0,
        ObjectId readObj1,
        ObjectId readObj2,
        ObjectId readObj3,
        ObjectId readObj4,
        ObjectId readObj5,
        ObjectId readObj6,
        ObjectId readObj7,
        Bit#(Log#(MaxTxnWriteObjs)) writeObjsCount,
        ObjectId writeObj0,
        ObjectId writeObj1,
        ObjectId writeObj2,
        ObjectId writeObj3,
        ObjectId writeObj4,
        ObjectId writeObj5,
        ObjectId writeObj6,
        ObjectId writeObj7,
    );
    /*
    For fake transactions, a trigger is needed to send transactions to Puppetmaster.
    */
    method Action triggerDriver();
    /*
    For host to report that it has completed a transaction
    */
    method Action reportWorkDone(TransactionId transactionId);
endinterface

/*
Connectal hardware-to-software interface.
*/
interface H2SMessage;
    method Action configData(TopConfig cfg);
    method Action transactionScheduled(TransactionId transactionId);
endinterface
