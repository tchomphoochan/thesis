ifndef CONNECTALDIR
$(error CONNECTALDIR variable is not defined, aborting build)
endif
ifndef BOARD
$(error BOARD variable is not defined, aborting build)
endif
ifndef PMHW_DIR
$(error PMHW_DIR variable is not defined, aborting build)
endif

SOURCES=src/main.cpp generated/obj/connectal.so
INCLUDE_DIR=./include

TMP_PMHW_DIR = ./tmp/pmhw
GENERATED_INCLUDE = ./generated/include
GENERATED_OBJ = ./generated/obj

CXXFLAGS += -I$(INCLUDE_DIR) -I$(GENERATED_INCLUDE)

EXE = ./bin/main

.PHONY: all
all: $(EXE)

$(EXE): $(SOURCES)
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(EXE)

generated/obj/connectal.so:
	$(error Connectal-related files have not been generated. Run "make generate" first.)

.PHONY: generate
generate:
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
	  if ($$0 == "CPPFILES += db.cpp" || $$0 == "CPPFILES += main.cpp") { print "#" $$0 } \
	  else { print $$0 } \
	}' $(PMHW_DIR)/Makefile > $(TMP_PMHW_DIR)/Makefile
	# Run all necessary make targets in Puppetmaster
	make gen.$(BOARD) -C $(TMP_PMHW_DIR)
	make syntax.timestamp -C $(TMP_PMHW_DIR)/$(BOARD)
	make connectal.so -C $(TMP_PMHW_DIR)/$(BOARD)/jni -f Ubuntu.mk
	# Copy the files
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/connectal.so $(GENERATED_OBJ)
	cp $(TMP_PMHW_DIR)/$(BOARD)/jni/*.h $(GENERATED_INCLUDE)
	cp $(CONNECTALDIR)/cpp/*.h $(GENERATED_INCLUDE)

.PHONY: run
run:
	NOPROGRAM=1 $(EXE)

.PHONY: clean
clean:
	rm -fR bin obj tmp

.PHONY: mrproper
mrproper: clean
	rm -fR $(GENERATED_INCLUDE) $(GENERATED_OBJ)
