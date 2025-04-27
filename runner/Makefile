ifndef CONNECTALDIR
$(error CONNECTALDIR variable is not defined, aborting build)
endif
ifndef BOARD
$(error BOARD variable is not defined, aborting build)
endif
ifndef PMHW_DIR
$(error PMHW_DIR variable is not defined, aborting build)
endif

TMP_PMHW_DIR = ./tmp/pmhw
GENERATED_INCLUDE = ./generated/include
GENERATED_OBJ = ./generated/obj

SOURCES=$(wildcard src/*) $(GENERATED_OBJ)/connectal.so
INCLUDE_DIR=./include

CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_INCLUDE)

EXE = ./bin/main

.PHONY: all
all: $(EXE)

$(EXE): $(SOURCES)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(EXE)

$(GENERATED_OBJ)/connectal.so:
	$(error Connectal-related files have not been generated. Run "make generate" first.)

.PHONY: generate
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

.PHONY: make-tmp
make-tmp:
	# Create directories
	rm -fR tmp/
	mkdir -p $(GENERATED_INCLUDE)
	mkdir -p $(GENERATED_OBJ)
	mkdir -p $(TMP_PMHW_DIR)
	# Copy Puppetmaster source but remove main.cpp
	cp -r $(PMHW_DIR)/* $(TMP_PMHW_DIR)
	rm -f $(TMP_PMHW_DIR)/main.cpp
	rm -f $(TMP_PMHW_DIR)/db.cpp
	awk '{ \
	  if ($$1 == "CPPFILES" && $$2 == "+=" && ($$3 ~ /^db\.cpp/ || $$3 ~ /^main\.cpp/)) { print "#" $$0 } \
	  else { print $$0 } \
	}' $(PMHW_DIR)/Makefile > $(TMP_PMHW_DIR)/Makefile

.PHONY: run
ifneq ($(BOARD), verilator)
run: $(EXE)
	NOPROGRAM=1 $(EXE)
else
run: $(EXE)
	$(EXE)
endif

.PHONY: clean
clean:
	rm -fR bin obj tmp
	rm -f SWSOCK* *.out

.PHONY: mrproper
mrproper: clean
	rm -fR $(GENERATED_INCLUDE) $(GENERATED_OBJ)
