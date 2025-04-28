# Require variables
ifndef CONNECTALDIR
$(error CONNECTALDIR variable is not defined, aborting build)
endif
ifndef BOARD
$(error BOARD variable is not defined, aborting build)
endif
ifndef PMHW_DIR
$(error PMHW_DIR variable is not defined, aborting build)
endif
ifndef SIM
$(error SIM variable is not defined (should be 0 or 1 or 2))
endif
ifndef WORK_SIMULATION_US
$(error WORK_SIMULATION_US variable is not defined, aborting build)
endif

# Directories
TMP_PMHW_DIR = ./tmp/pmhw
GENERATED_ROOT = ./generated
GENERATED_INCLUDE = $(GENERATED_ROOT)/include
GENERATED_OBJ = $(GENERATED_ROOT)/obj

# Sources and includes
INCLUDE_DIR = ./include
SOURCE_DIR = ./src
OBJ_DIR = ./obj
BIN_DIR = ./bin
SOURCES = $(SOURCE_DIR)/main.c
OBJECTS = 

ifeq ($(SIM),0)
SOURCES += $(SOURCE_DIR)/pmhw.cpp
SOURCES += $(GENERATED_OBJ)/connectal.so
endif

ifeq ($(SIM),1)
SOURCES += $(SOURCE_DIR)/pmhw_sim.c
endif

ifeq ($(SIM),2)
SOURCES += $(SOURCE_DIR)/pmhw_sim_bloom.c
endif

CC = gcc
CXX = g++
CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_INCLUDE) -O2 -Wall -pthread -g -DWORK_SIMULATION_US=$(WORK_SIMULATION_US)
CFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_INCLUDE) -O2 -Wall -pthread -g -DWORK_SIMULATION_US=$(WORK_SIMULATION_US)

# Executable
EXE = ./bin/main
ANALYZE_EXE = ./bin/analyze

.PHONY: all
all: $(EXE) $(ANALYZE_EXE)

$(EXE): $(SOURCES)
	# Create output directory
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(EXE)

$(ANALYZE_EXE): ./src/analyze.c
	mkdir -p bin
	$(CC) $(CFLAGS) ./src/analyze.c -o $(ANALYZE_EXE)

$(GENERATED_OBJ)/connectal.so:
	$(error Connectal-related files have not been generated. Run "make generate" first.)

.PHONY: generate
ifneq ($(SIM), 0)
generate:
	$(error "It does not make sense to generate Connectal files when SIM != 0")
else
ifneq ($(BOARD), verilator)
generate: | make-tmp
	# Run make-tmp first (see above) to generate the files
	# Run all necessary make targets in Puppetmaster
	make gen.$(BOARD) -C $(TMP_PMHW_DIR)
	make syntax.timestamp -C $(TMP_PMHW_DIR)/$(BOARD)
	make connectal.so -C $(TMP_PMHW_DIR)/$(BOARD)/jni -f Ubuntu.mk
	# Copy the files
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/connectal.so $(GENERATED_OBJ)
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/*.h $(GENERATED_INCLUDE)
	cp $(CONNECTALDIR)/cpp/*.h $(GENERATED_INCLUDE)
else
generate: | make-tmp
	# Run make-tmp first (see above) to generate the files
	# Run all necessary make targets in Puppetmaster
	# (New thing from other boards: vlsim)
	make gen.$(BOARD) -C $(TMP_PMHW_DIR)
	make vlsim -C $(TMP_PMHW_DIR)/$(BOARD)
	make syntax.timestamp -C $(TMP_PMHW_DIR)/$(BOARD)
	make connectal.so -C $(TMP_PMHW_DIR)/$(BOARD)/jni -f Ubuntu.mk
	# Copy the files
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/connectal.so $(GENERATED_OBJ)
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/*.h $(GENERATED_INCLUDE)
	cp $(TMP_PMHW_DIR)/$(BOARD)/bin/vlsim $(GENERATED_OBJ)
	cp $(TMP_PMHW_DIR)/$(BOARD)/bin/libconnectal-sim.so $(GENERATED_OBJ)
	cp $(CONNECTALDIR)/cpp/*.h $(GENERATED_INCLUDE)
endif
endif

.PHONY: make-tmp
make-tmp:
	# Create directories
	rm -fR tmp/
	mkdir -p $(GENERATED_INCLUDE)
	mkdir -p $(GENERATED_OBJ)
	mkdir -p $(TMP_PMHW_DIR)
	# Copy Puppetmaster source but remove code in the root directory
	cp -r $(PMHW_DIR)/* $(TMP_PMHW_DIR)
	rm -f $(TMP_PMHW_DIR)/*.cpp
	# Remove mentions in the Makefile too
	awk '{ \
	  if ($$1 == "CPPFILES" && $$2 == "+=" && ($$3 ~ /^db\.cpp/ || $$3 ~ /^main\.cpp/)) { print "#" $$0 } \
	  else { print $$0 } \
	}' $(PMHW_DIR)/Makefile > $(TMP_PMHW_DIR)/Makefile
	# Clear out any board folder if it exists 
	rm -fR $(TMP_PMHW_DIR)/$(BOARD)

.PHONY: run
ifneq ($(BOARD), verilator)
run:
	make clean
	make all
	# Run the executable with NOPROGRAM if not verilator
	NOPROGRAM=1 $(EXE) transactions.csv 20 2> err.txt > out.txt
	./bin/analyze transactions.csv out.txt 8 $(WORK_SIMULATION_US)
else
run:
	make clean
	make all
	# Run normally if verilator
	$(EXE) transactions.csv 20 2> err.txt > out.txt
	./bin/analyze transactions.csv out.txt 8 $(WORK_SIMULATION_US)
endif

.PHONY: clean
clean:
	# Clean build artifacts
	rm -fR bin obj tmp
	rm -f SWSOCK* *.out

.PHONY: mrproper
mrproper: clean
	# Deep clean: also remove generated root
	rm -fR $(GENERATED_ROOT)

