import BasicTypes::*;
import MainTypes::*;
import GetPut::*;
import FIFO::*;
import SpecialFIFOs::*;
import Vector::*;
import BRAMFIFO::*;

/*
Input side of the Puppetmaster scheduler.
*/
interface TxnDriver;
    /*
    Transactions to be made available to Puppetmaster.
    */
    interface Get#(Transaction) transactions;
endinterface


/*
Input MUX
*/
interface TxnDriverMux#(numeric type n);
    method Action select(TIndex#(n) idx);
    method TIndex#(n) selected;
    interface TxnDriver txnDriver;
endinterface

module mkTxnDriverMux(Vector#(n, TxnDriver) txnDrivers, TxnDriverMux#(n) ifc);
    Reg#(TIndex#(n)) index <- mkRegU;

    method Action select(TIndex#(n) idx);
        index <= idx;
    endmethod

    method TIndex#(n) selected = index;

    interface txnDriver = txnDrivers[index];
endmodule


/*
Real transaction driver that takes data from the host CPU.
*/
interface RealTxnDriver;
    interface Put#(Transaction) fromHost;
    interface TxnDriver txnDriver;
endinterface

module mkRealTxnDriver(RealTxnDriver);
    FIFO#(Transaction) hostFF <- mkBypassFIFO;

    interface fromHost = toPut(hostFF);
    interface TxnDriver txnDriver;
        interface transactions = toGet(hostFF);
    endinterface
endmodule

/*
Fake transaction driver
*/
interface FakeTxnDriver;
    method Action resetState;
    interface Put#(Transaction) fromHost;
    method Action setStreamOpen(Bool ok);
    interface TxnDriver txnDriver;
endinterface

// 432 * 2^15 = 14.1 Mb
typedef 15 LogFakeTxnBRAMSize;
typedef TExp#(LogFakeTxnBRAMSize) FakeTxnBRAMSize;
typedef Bit#(LogFakeTxnBRAMSize) FakeTxnBRAMAddr;

module mkFakeTxnDriver(FakeTxnDriver);
    Reg#(Timestamp) cycle <- mkReg(0);
    FIFO#(Transaction) txns <- mkSizedBRAMFIFO(valueOf(FakeTxnBRAMSize));
    Reg#(Bool) started <- mkReg(False);
    Reg#(FakeTxnBRAMAddr) outCount <- mkReg(0);

    (* no_implicit_conditions, fire_when_enabled *)
    rule tick;
        cycle <= cycle+1;
    endrule

    method Action resetState;
        txns.clear();
        started <= False;
        outCount <= 0;
        $fdisplay(stderr, "[%0d] FakeTxnDriver: resetState", cycle);
    endmethod

    interface Put fromHost;
        method Action put(Transaction txn);
            txns.enq(txn);
            $fdisplay(stderr, "[%0d] FakeTxnDriver: enqueued ", cycle, fshow(txn));
        endmethod
    endinterface

    method Action setStreamOpen(Bool ok);
        started <= ok;
        $fdisplay(stderr, "[%0d] FakeTxnDriver: setStreamOpen(%b)", cycle, ok);
    endmethod

    interface TxnDriver txnDriver;
        interface Get transactions;
            method ActionValue#(Transaction) get if (started);
                let txn <- toGet(txns).get;
                outCount <= outCount+1;
                $fdisplay(stderr, "[%0d] FakeTxnDriver: Txn returned ", cycle, fshow(txn));
                return txn;
            endmethod
        endinterface
    endinterface
endmodule
