WRAPPER_DIR = deps/wrapper
GENERATED_DIR = $(WRAPPER_DIR)/output/$(BOARD)
PMHW_FILES = $(addprefix $(GENERATED_DIR)/, pmhw.h pmhw.so)

INCLUDE_DIR = ./include
SRC_DIR = ./src
BIN_DIR = ./bin

DEFINES = -D_GNU_SOURCE -DWORK_SIMULATION_US=$(WORK_SIMULATION_US)

CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread $(DEFINES) -g
CFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread $(DEFINES) -g

LIB_DEPS = -l:pmhw.so

ifneq ($(BOARD), $(filter $(BOARD), sim sim_bloom))
LIB_DEPS += -l:connectal.so
endif

ifeq ($(BOARD), verilator)
LIB_DEPS += -l:libconnectal-sim.so
endif

.PHONY: all
all: $(BIN_DIR)/main $(BIN_DIR)/analyze

ifeq ($(BOARD), )
.PHONY: $(BIN_DIR)/main
$(BIN_DIR)/main:
	$(error BOARD variable is not defined, aborting build)
else
$(BIN_DIR)/main: $(SRC_DIR)/main.c $(PMHW_FILES) $(BIN_DIR)/board.txt 
	@mkdir -p bin
	$(CC) $(CFLAGS) $< $(LIB_DEPS) -o $@
endif

# Check whether BOARD equals existing board.txt. If not, make this target PHONY to force it to run,
# causing $(BIN_MAIN)/main to also be re-run.
ifneq ($(BOARD), $(shell [ -f $(BIN_DIR)/board.txt ] && cat $(BIN_DIR)/board.txt))
.PHONY: $(BIN_DIR)/board.txt
endif
$(BIN_DIR)/board.txt:
	@mkdir -p bin
	echo $(BOARD) > $(BIN_DIR)/board.txt

$(BIN_DIR)/analyze: $(SRC_DIR)/analyze.c
	$(CC) $(CFLAGS) $< -o $@

$(PMHW_FILES):
	$(error Manually "BOARD=... make -C deps/wrapper" to generate the necessary files before running this)

.PHONY: clean
clean:
	rm -fR $(BIN_DIR)
	rm -f SWSOCK*

