typedef Bit#(32) Index;
typedef Bit#(32) Count;
typedef Bit#(64) Cycle;

interface PingPongRequest;
  method Action start(Count cnt);
  method Action ping(Bool needsResponse);
endinterface

interface HwTop;
    interface PingPongRequest pingPongRequest;
endinterface

interface PingPongIndication;
  method Action pong();
  method Action reportTime(Cycle duration);
endinterface

module mkHwTop#(PingPongIndication pingPongIndication)(HwTop);
  Reg#(Count) n <- mkReg(0);
  Reg#(Cycle) cycle <- mkReg(0);
  Reg#(Maybe#(Cycle)) startCycle <- mkReg(Invalid);

  rule tick;
    cycle <= cycle+1;
  endrule

  rule stream_pongs if (n > 0);
    n <= n-1;
    pingPongIndication.pong();
  endrule

  rule send_cycle_count if (startCycle matches tagged Valid .sc &&& n == 0);
    pingPongIndication.reportTime(cycle - sc);
    startCycle <= Invalid;
  endrule

  interface PingPongRequest pingPongRequest;
    method Action start(Count cnt) if (n == 0);
      n <= cnt;
      startCycle <= Valid(cycle);
    endmethod

    method Action ping(Bool needsResponse);
      if (needsResponse) begin
        pingPongIndication.pong();
      end
    endmethod
  endinterface

endmodule