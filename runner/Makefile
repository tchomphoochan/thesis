WRAPPER_DIR = deps/wrapper
GENERATED_DIR = $(WRAPPER_DIR)/output/$(BOARD)
PMHW_FILES = $(addprefix $(GENERATED_DIR)/, pmhw.h pmhw.so)

INCLUDE_DIR = ./include
SRC_DIR = ./src
BIN_DIR = ./bin

DEFINES = -D_GNU_SOURCE -DWORK_SIMULATION_US=$(WORK_SIMULATION_US)

CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread $(DEFINES) -g
CFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread $(DEFINES) -g

.PHONY: all
all: $(BIN_DIR)/main

ifeq ($(BOARD), )
.PHONY: $(BIN_DIR)/main
$(BIN_DIR)/main:
	$(error BOARD variable is not defined, aborting build)
else
$(BIN_DIR)/main: $(SRC_DIR)/main.c $(PMHW_FILES)
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -l:pmhw.so -o $@
	echo $(BOARD) > $(BIN_DIR)/board.txt
endif

$(PMHW_FILES):
	$(error Manually "BOARD=... make -C deps/wrapper" to generate the necessary files before running this)

.PHONY: clean
clean:
	rm -fR $(BIN_DIR)

