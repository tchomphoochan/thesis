BOARD?=vcu108
VIVADO_VERSION?=$(shell ./scripts/get-vivado-version.sh)
TOP_LEVEL=mkTop_vcu108

SRC=./src

BUILD_DIR=./build
INFO_DIR=$(BUILD_DIR)/info
VERILOG_DIR=$(BUILD_DIR)/verilog
BLUESPEC_DIR=$(BUILD_DIR)/bluespec
SIM_DIR=$(BUILD_DIR)/sim

# -------------------------
# Miscellaneous targets
# -------------------------

.PHONY: all
all:
	$(error Do not use "all" target)

.PHONY: dirs
dirs:
	@mkdir -p $(INFO_DIR)
	@mkdir -p $(VERILOG_DIR)
	@mkdir -p $(BLUESPEC_DIR)
	@mkdir -p $(SIM_DIR)

.PHONY: clean
clean:
	rm -fR build
	rm -f $(SRC)/*.ba $(SRC)/*.bo $(SRC)/*.sched

# -------------------------
# Generate Verilog using bsc
# -------------------------

BSC_SUPPRESSED_WARNINGS=G0117:G0046:G0020
BSC_FLAGS=\
	+RTS -Ksize -RTS\
  -vdir $(VERILOG_DIR) \
  -bdir $(BLUESPEC_DIR) \
  -simdir $(SIM_DIR)\
  -info-dir $(INFO_DIR) \
  -aggressive-conditions -show-schedule \
  -suppress-warnings $(BSC_SUPPRESSED_WARNINGS) \
  -p +:%/Libraries/FPGA/Xilinx:$(SRC)

BSC_SIM_FLAGS=-sim
BSC_SYNTH_FLAGS=-remove-dollar -verilog

SRCS=$(wildcard $(SRC)/*.bsv)
TOP_LEVEL_VERILOG_FILE=$(BUILD_DIR)/verilog/$(TOP_LEVEL).v
	
$(TOP_LEVEL_VERILOG_FILE): $(BUILD_DIR)/verilog/%.v: $(SRC)/%.bsv $(SRCS)
	make dirs
	bsc $(BSC_FLAGS) -g $* -u $(SRC)/$*.bsv

# -------------------------
# Manual Verilog inspection targets
# -------------------------

VERILOG_NAMES=Summary SummarySynth
VERILOG_FILES=$(foreach test, $(VERILOG_NAMES), $(VERILOG_DIR)/mk$(test).v)

$(VERILOG_FILES): $(VERILOG_DIR)/mk%.v: $(SRCS)
	make dirs
	bsc $(BSC_FLAGS) $(BSC_SYNTH_FLAGS) -g mk$* -u $(SRC)/$*.bsv

# -------------------------
# Test targets
# -------------------------

TEST_NAMES=SummaryTest PuppetmasterTest
TEST_EXES=$(foreach test, $(TEST_NAMES), $(SIM_DIR)/mk$(test))

$(TEST_EXES): $(SIM_DIR)/mk%: $(SRCS)
	make dirs
	bsc $(BSC_FLAGS) $(BSC_SIM_FLAGS) -g mk$* -u $(SRC)/$*.bsv
	bsc $(BSC_FLAGS) $(BSC_SIM_FLAGS) -e mk$* -o $(SIM_DIR)/mk$*

# -------------------------
# Generate bitstreams from Verilog
# -------------------------

SUPPORTED_BOARDS:=vcu108
BUILD_TARGETS:=$(foreach board, $(SUPPORTED_BOARDS), build.$(board))
FLASH_TARGETS:=$(foreach board, $(SUPPORTED_BOARDS), flash.$(board))
BITSTREAM_TARGETS:=$(foreach board, $(SUPPORTED_BOARDS), $(BUILD_DIR)/$(board)/final.bit)


.PHONY: $(BUILD_TARGETS)
$(BUILD_TARGETS): build.%: $(BUILD_DIR)/%/final.bit


.PHONY: $(FLASH_TARGETS)
$(FLASH_TARGETS): flash.%:
	@echo "Attempting to flash $(BUILD_DIR)/$*/final.bit without re-synthesizing..."
	fpgajtag $(BUILD_DIR)/$*/final.bit

$(BITSTREAM_TARGETS): $(BUILD_DIR)/%/final.bit: $(TOP_LEVEL_VERILOG_FILE) tcl/build.tcl
	BOARD=$(BOARD) VIVADO_VERSION=$(VIVADO_VERSION) TOP_LEVEL=$(TOP_LEVEL) \
	  vivado -mode batch -source tcl/build.tcl
