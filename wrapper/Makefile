default: all

# ---------------------
# First step: Extract all the necessary files from Connectal-based projext
# ---------------------

TMP_DIR = ./tmp
TMP_PMHW_DIR = $(TMP_DIR)/pmhw
GENERATED_ROOT = ./generated
GENERATED_DIR = $(GENERATED_ROOT)/$(BOARD)

# Surrogate for checking whether Connectal files have been generated
$(GENERATED_DIR)/connectal.so:
	$(error Connectal-related files have not been generated. Run "make generate" first.)

SIM_TYPES = sim sim_bloom sim_full_bloom sim_sort

# Generate Conectal-related files
.PHONY: generate
generate:
ifeq ($(BOARD), )
	$(error BOARD variable is not defined, aborting build)
else ifeq ($(BOARD), $(filter $(BOARD), $(SIM_TYPES)))
	$(error Cannot generate Connectal files with with BOARD = $(BOARD))
else ifeq ($(CONNECTALDIR), )
	$(error CONNECTALDIR variable is not defined, aborting build)
else ifeq ($(PMHW_DIR), )
	$(error PMHW_DIR variable is not defined, aborting build)
else

	# Clean up
	rm -fR $(TMP_DIR) $(GENERATED_DIR)
	mkdir -p $(TMP_PMHW_DIR)
	mkdir -p $(GENERATED_DIR)

	# Copy Puppetmaster source but remove code in the root directory
	cp -r $(PMHW_DIR)/* $(TMP_PMHW_DIR)
	
	# Remove lingering CPP files
	rm -f $(TMP_PMHW_DIR)/*.cpp

	# Remove mentions in the Makefile too
	awk '{ \
	  if ($$1 == "CPPFILES" && $$2 == "+=" && ($$3 ~ /^db\.cpp/ || $$3 ~ /^main\.cpp/)) { print "#" $$0 } \
	  else { print $$0 } \
	}' $(PMHW_DIR)/Makefile > $(TMP_PMHW_DIR)/Makefile

	# Clear out any board folder if it exists 
	rm -fR $(TMP_PMHW_DIR)/$(BOARD)

	# Run all necessary make targets in Puppetmaster.
	# Verilator is a special case in that we must also generate vlsim.
	make gen.$(BOARD) -C $(TMP_PMHW_DIR)
ifeq ($(BOARD), verilator)
	make vlsim -C $(TMP_PMHW_DIR)/$(BOARD)
endif
	make syntax.timestamp -C $(TMP_PMHW_DIR)/$(BOARD)
	make connectal.so -C $(TMP_PMHW_DIR)/$(BOARD)/jni -f Ubuntu.mk
	
	# Copy the files
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/connectal.so $(GENERATED_DIR)
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/*.h $(GENERATED_DIR)
ifeq ($(BOARD), verilator)
	cp $(TMP_PMHW_DIR)/$(BOARD)/bin/vlsim $(GENERATED_DIR)
	cp $(TMP_PMHW_DIR)/$(BOARD)/bin/libconnectal-sim.so $(GENERATED_DIR)
endif
	cp $(CONNECTALDIR)/cpp/*.h $(GENERATED_DIR)
endif

# ---------------------
# Second step: Actually build pmhw wrapper
# Need to use correct sources and link to correct Connectal libraries
# depending on the board
# ---------------------

SRC_DIR = ./src
OBJ_DIR = ./obj/$(BOARD)
INCLUDE_DIR = ./include

CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread -shared -fpic
CFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_DIR) -L$(GENERATED_DIR) -O2 -Wall -pthread -shared -fpic

COMP = 
SOURCES =
CONNECTAL_DEPS =

ifeq ($(BOARD), sim)
COMP = $(CC) $(CFLAGS)
SOURCES += $(SRC_DIR)/pmhw_sim.c

else ifeq ($(BOARD), sim_bloom)
COMP = $(CC) $(CFLAGS)
SOURCES += $(SRC_DIR)/pmhw_sim_bloom.c

else ifeq ($(BOARD), pmhw_sim_bloom)
COMP = $(CC) $(CFLAGS)
SOURCES += $(SRC_DIR)/pmhw_sim_full_bloom.c

else ifeq ($(BOARD), sim_full_bloom)
COMP = $(CC) $(CFLAGS)
SOURCES += $(SRC_DIR)/pmhw_sim_sort.c

else ifeq ($(BOARD), verilator)
COMP = $(CXX) $(CXXFLAGS)
SOURCES += $(SRC_DIR)/pmhw.cpp
CONNECTAL_DEPS += $(GENERATED_DIR)/connectal.so $(GENERATED_DIR)/libconnectal-sim.so $(GENERATED_DIR)/vlsim
CXXFLAGS += -l:connectal.so -l:libconnectal-sim.so
CFLAGS += -l:connectal.so -l:libconnectal-sim.so

else ifneq ($(BOARD), )
COMP = $(CXX) $(CXXFLAGS)
SOURCES += $(SRC_DIR)/pmhw.cpp
CONNECTAL_DEPS += $(GENERATED_DIR)/connectal.so
CXXFLAGS += -l:connectal.so
CFLAGS += -l:connectal.so
endif

OBJ = $(OBJ_DIR)/pmhw.so

ifeq ($(BOARD), )
.PHONY: $(OBJ)
$(OBJ):
	$(error BOARD variable is not defined, aborting build)
else
$(OBJ): $(SOURCES) $(CONNECTAL_DEPS)
	@mkdir -p $(OBJ_DIR)
	$(COMP) $(SOURCES) -o $@
endif

# ---------------------
# Final step: Gather files
# ---------------------

OUTPUT_DIR = output/$(BOARD)

.PHONY: output
output: $(OBJ)
	@mkdir -p $(OUTPUT_DIR)
	@cp $(OBJ) $(OUTPUT_DIR)/
	@cp $(INCLUDE_DIR)/pmhw.h $(OUTPUT_DIR)/
	@if [ -n "$(CONNECTAL_DEPS)" ]; then \
	  cp $(CONNECTAL_DEPS) $(OUTPUT_DIR)/; \
	fi
	@echo Your output is available at $(OUTPUT_DIR)

.PHONY: all
all:
ifneq ($(BOARD), $(filter $(BOARD), $(SIM_TYPES)))
	make generate
endif
	make output

# ---------------------
# Extra stuff
# ---------------------

BIN_DIR = ./bin

$(BIN_DIR)/spsc_bench: $(SRC_DIR)/spsc_bench.c
	mkdir -p bin
	$(CC) -O2 -Wall -pthread -I$(INCLUDE_DIR) $< -o $@

# ---------------------
# Clean targets
# ---------------------

.PHONY: clean
clean:
	# Clean build artifacts
	rm -fR bin obj output

.PHONY: mrproper
mrproper: clean
	# Deep clean: also remove generated root
	rm -fR $(GENERATED_DIR)

