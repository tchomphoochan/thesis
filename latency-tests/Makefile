ifndef CONNECTALDIR
$(error CONNECTALDIR variable is not defined, aborting build)
endif

S2H_INTERFACES = PingPongRequest:HwTop.pingPongRequest
H2S_INTERFACES = HwTop:PingPongIndication

BSVFILES = HwTop.bsv
CPPFILES = main.cpp

CONNECTALFLAGS += --mainclockperiod=4 --derivedclockperiod=4 --pcieclockperiod=4
CONNECTALFLAGS += --nonstrict
CONNECTALFLAGS += --verilatorflags="--no-timing"

include $(CONNECTALDIR)/Makefile.connectal
