# Define the main output files
# ===========================
PAINLESS_OUTPUT := backpainless
DEBUG_OUTPUT := $(PAINLESS_OUTPUT)_debug
RELEASE_OUTPUT := $(PAINLESS_OUTPUT)_release

# Compiler and flags
# ==================
CC := gcc
CXX := g++
COMMON_FLAGS := $(shell mpic++ --showme:compile) -fopenmp

# Check GCC version to determine proper C++ standard flag
GCC_MAJOR := $(shell gcc -dumpversion)

# Set appropriate C++20 flag based on GCC version
ifeq ($(shell expr $(GCC_MAJOR) \< 8), 1)
    $(error GCC version $(gcc --version) does not support C++20. Version 8 or higher required.)
else ifeq ($(shell expr $(GCC_MAJOR) \< 10), 1)
    # GCC 8 or 9 uses -std=c++2a
    CPP_STD_FLAG := -std=c++2a
else
    # GCC 10+ uses -std=c++20
    CPP_STD_FLAG := -std=c++20
endif

# Add standard flag to common flags
COMMON_FLAGS += $(CPP_STD_FLAG)

# Define debug and release flags 
DEBUG_FLAGS := $(COMMON_FLAGS) -g3 -O0 #-fsanitize=thread#-Wall -Wextra
RELEASE_FLAGS := $(COMMON_FLAGS) -O3 -DNDEBUG


# Directories
# ===========
SRC_DIR := src
SOLVERS_DIR := solvers
LIBS_DIR := libs
BUILD_DIR := build
DEBUG_BUILD_DIR := $(BUILD_DIR)/debug
RELEASE_BUILD_DIR := $(BUILD_DIR)/release

# Solver and library directories
# ==============================
MAPLE_BUILD := $(SOLVERS_DIR)/mapleCOMSPS/build/release/lib
GLUCOSE_BUILD := $(SOLVERS_DIR)/glucose/parallel
MINISAT_BUILD := $(SOLVERS_DIR)/minisat/build/release/lib
LINGELING_BUILD := $(SOLVERS_DIR)/lingeling
YALSAT_BUILD := $(SOLVERS_DIR)/yalsat
TASSAT_BUILD := $(SOLVERS_DIR)/tassat
KISSAT_BUILD := $(SOLVERS_DIR)/kissat/build
CADICAL_BUILD := $(SOLVERS_DIR)/cadical/build
CMS_DIR := $(SOLVERS_DIR)/cryptominisat
CMS_BUILD := $(CMS_DIR)/build
KISSATMAB_BUILD := $(SOLVERS_DIR)/kissat_mab/build
KISSATINC_BUILD := $(SOLVERS_DIR)/kissat-inc/build
KISSATGASPI_BUILD := $(SOLVERS_DIR)/solvers/GASPIKISSAT/build

M4RI_DIR := $(LIBS_DIR)/m4ri-20200125

# Define dependencies
# ===================
# CaDiCaL is the SAT engine of the CadiBack backbone solvers (src/solvers/CDCL/cadiback/), CryptoMiniSat the one of
# DiverseBackboneSearch (src/solvers/CDCL/cryptominisat/, native XOR constraints).
# The other vendored solvers in solvers/ can still be built with their own targets (make kissat, make solvers, ...).
DEPENDENCIES := $(CADICAL_BUILD)/libcadical.a $(CMS_BUILD)/libcryptominisat5.a

# Library flags
# =============
LIBS := -l:libcadical.a -L$(CADICAL_BUILD) \
		-l:libcryptominisat5.a -L$(CMS_BUILD) \
		-lpthread -lz -lm $(shell mpic++ --showme:link)

# Include directories
# ===================
INCLUDES := -I$(SRC_DIR) \
            -I$(SOLVERS_DIR)

# Source files
# ============
# src/disabled/ holds code kept for later reintegration (PRS, SBVA, PortfolioPRS), it is not compiled
SRCS := $(shell find $(SRC_DIR) -name "*.cpp" -not -path "*/.ignore/*" -not -path "*/disabled/*")
DEBUG_OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(DEBUG_BUILD_DIR)/%.o)
RELEASE_OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(RELEASE_BUILD_DIR)/%.o)

# All target
# ==============
.PHONY: all
all:
	$(MAKE) cadical && $(MAKE) cryptominisat && $(MAKE) painless

.DEFAULT_GOAL := all

# Painless target
# ==============
.PHONY: painless debug release

painless: debug release

# Create build directories
# ========================
$(shell mkdir -p $(DEBUG_BUILD_DIR) $(RELEASE_BUILD_DIR))

# Main targets
# ============
debug: $(DEBUG_BUILD_DIR)/$(DEBUG_OUTPUT)
	ln -sf $(DEBUG_BUILD_DIR)/$(DEBUG_OUTPUT) backpainlessd

release: $(RELEASE_BUILD_DIR)/$(RELEASE_OUTPUT)
	ln -sf $(RELEASE_BUILD_DIR)/$(RELEASE_OUTPUT) backpainless

.PHONY: test
test: release
	python3 tests/run_tests.py

$(DEBUG_BUILD_DIR)/$(DEBUG_OUTPUT): $(DEBUG_OBJS) $(DEPENDENCIES)
	$(CXX) -o $@ $(DEBUG_OBJS) $(DEBUG_FLAGS) $(INCLUDES) $(LIBS)

$(RELEASE_BUILD_DIR)/$(RELEASE_OUTPUT): $(RELEASE_OBJS) $(DEPENDENCIES)
	$(CXX) -o $@ $(RELEASE_OBJS) $(RELEASE_FLAGS) $(INCLUDES) $(LIBS)

# Pattern rules for object files
# ==============================
# -MMD -MP write a .d file next to each object with the headers it includes, so that editing a header rebuilds every
# object that depends on it (otherwise objects keep a stale class layout)
$(DEBUG_BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ -MMD -MP $(DEBUG_FLAGS) $(INCLUDES)

$(RELEASE_BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ -MMD -MP $(RELEASE_FLAGS) $(INCLUDES)

-include $(DEBUG_OBJS:.o=.d) $(RELEASE_OBJS:.o=.d)

# Simplified library targets
# ==========================
.PHONY: minisat glucose lingeling kissat kissat_mab kissat_inc kissat_gaspi yalsat cadical cryptominisat maple m4ri tassat

solvers: minisat glucose lingeling kissat kissat_mab kissat_inc kissat_gaspi yalsat cadical maple tassat

libs: m4ri

minisat: $(MINISAT_BUILD)/libminisat.a
glucose: $(GLUCOSE_BUILD)/libglucose.a
lingeling: $(LINGELING_BUILD)/liblgl.a
kissat: $(KISSAT_BUILD)/libkissat.a
kissat_mab: $(KISSATMAB_BUILD)/libkissat_mab.a
kissat_inc: $(KISSATINC_BUILD)/libkissat_inc.a
# kissat_gaspi: $(KISSATGASPI_BUILD)/libgkissat.a
yalsat: $(YALSAT_BUILD)/libyals.a
tassat: $(TASSAT_BUILD)/libtas.a
cadical: $(CADICAL_BUILD)/libcadical.a
cryptominisat: $(CMS_BUILD)/libcryptominisat5.a
maple: $(MAPLE_BUILD)/libmapleCOMSPS.a
m4ri: $(M4RI_DIR)/.libs/libm4ri.a

# Library targets
# ===============
$(MINISAT_BUILD)/libminisat.a:
	$(MAKE) -C $(SOLVERS_DIR)/minisat

$(GLUCOSE_BUILD)/libglucose.a:
	cd $(SOLVERS_DIR)/glucose && $(MAKE) parallel/libglucose.a

$(LINGELING_BUILD)/liblgl.a: $(YALSAT_BUILD)/libyals.a
	cd $(SOLVERS_DIR)/lingeling && ./configure.sh
	$(MAKE) -C $(SOLVERS_DIR)/lingeling liblgl.a

$(KISSAT_BUILD)/libkissat.a:
	cd $(SOLVERS_DIR)/kissat && bash ./configure --no-proofs
	$(MAKE) -C $(SOLVERS_DIR)/kissat

$(KISSATMAB_BUILD)/libkissat_mab.a:
	cd $(SOLVERS_DIR)/kissat_mab && bash ./configure --no-proofs
	$(MAKE) -C $(SOLVERS_DIR)/kissat_mab

$(KISSATINC_BUILD)/libkissat_inc.a:
	cd $(SOLVERS_DIR)/kissat-inc && bash ./configure --no-proofs
	$(MAKE) -C $(SOLVERS_DIR)/kissat-inc

# $(KISSATGASPI_BUILD)/libgkissat.a:
# 	cd $(SOLVERS_DIR)/GASPIKISSAT && bash ./configure
# 	$(MAKE) -C $(SOLVERS_DIR)/GASPIKISSAT

$(YALSAT_BUILD)/libyals.a:
	cd $(SOLVERS_DIR)/yalsat && bash ./configure.sh
	$(MAKE) -C $(SOLVERS_DIR)/yalsat

$(TASSAT_BUILD)/libtas.a:
	cd $(SOLVERS_DIR)/tassat && bash ./configure.sh
	$(MAKE) -C $(SOLVERS_DIR)/tassat

$(CADICAL_BUILD)/libcadical.a:
	cd $(SOLVERS_DIR)/cadical && bash ./configure
	$(MAKE) -C $(SOLVERS_DIR)/cadical

# CryptoMiniSat 5.11.21 (last release that does not depend on CaDiCaL/CadiBack, whose symbols would clash with the
# vendored CaDiCaL), built without cmake: the same sources and defines as its default cmake configuration (no Python,
# no SQLite, no BreakID, no GPU), with zlib, assertions off
CMS_SRCS := $(addprefix $(CMS_DIR)/src/, cnf.cpp frat.cpp propengine.cpp varreplacer.cpp clausecleaner.cpp \
	occsimplifier.cpp gatefinder.cpp subsumestrengthen.cpp clauseallocator.cpp sccfinder.cpp solverconf.cpp \
	distillerlong.cpp distillerlitrem.cpp distillerbin.cpp distillerlongwithimpl.cpp str_impl_w_impl.cpp \
	solutionextender.cpp completedetachreattacher.cpp searcher.cpp solver.cpp hyperengine.cpp subsumeimplicit.cpp \
	datasync.cpp reducedb.cpp bva.cpp intree.cpp searchstats.cpp xorfinder.cpp cardfinder.cpp cryptominisat_c.cpp \
	sls.cpp sqlstats.cpp vardistgen.cpp ccnr.cpp ccnr_cms.cpp lucky.cpp get_clause_query.cpp gaussian.cpp \
	packedrow.cpp matrixfinder.cpp oracle/oracle.cpp cryptominisat.cpp)
CMS_CSRCS := $(addprefix $(CMS_DIR)/src/, picosat/picosat.c picosat/version.c)
CMS_OBJS := $(CMS_SRCS:$(CMS_DIR)/src/%.cpp=$(CMS_BUILD)/%.o) $(CMS_CSRCS:$(CMS_DIR)/src/%.c=$(CMS_BUILD)/%.o) \
	$(CMS_BUILD)/GitSHA1.o
CMS_DEFINES := -DNDEBUG -DRDB0_ONLY_FEATURES -DTRACE -DUSE_ZLIB -I$(CMS_DIR) -I$(CMS_DIR)/src -w
CMS_CXXFLAGS := -std=c++17 -O3 -pthread $(CMS_DEFINES)

$(CMS_BUILD)/libcryptominisat5.a: $(CMS_OBJS)
	ar rcs $@ $^

$(CMS_BUILD)/GitSHA1.cpp: $(CMS_DIR)/src/GitSHA1.cpp.in
	@mkdir -p $(@D)
	sed -e 's/@GIT_SHA1@/4c9a6b6b459b7c1381744115ccfe744f1121794b/' -e 's/@PROJECT_VERSION@/5.11.21/' \
		-e 's/@[A-Za-z0-9_]*@//g' $< > $@

$(CMS_BUILD)/%.o: $(CMS_BUILD)/%.cpp
	$(CXX) -c $< -o $@ $(CMS_CXXFLAGS)

$(CMS_BUILD)/%.o: $(CMS_DIR)/src/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CMS_CXXFLAGS)

$(CMS_BUILD)/%.o: $(CMS_DIR)/src/%.c
	@mkdir -p $(@D)
	$(CC) -c $< -o $@ -O3 $(CMS_DEFINES)

$(MAPLE_BUILD)/libmapleCOMSPS.a:
	$(MAKE) -C $(SOLVERS_DIR)/mapleCOMSPS r

$(M4RI_DIR)/.libs/libm4ri.a:
	cd $(M4RI_DIR) && autoreconf --install && ./configure --enable-thread-safe
	$(MAKE) -C $(M4RI_DIR)

# Clean targets
# =============
.PHONY: clean cleanpainless cleansolvers clean cleanall
cleanpainless:
	rm -rf $(BUILD_DIR)
	rm -rf backpainless backpainlessd

cleansolvers:
	$(MAKE) clean -C $(SOLVERS_DIR)/kissat
	$(MAKE) clean -C $(SOLVERS_DIR)/kissat_mab
	$(MAKE) clean -C $(SOLVERS_DIR)/kissat-inc
	# $(MAKE) clean -C $(SOLVERS_DIR)/GASPIKISSAT
	$(MAKE) clean -C $(SOLVERS_DIR)/cadical
	rm -rf $(CMS_BUILD)
	$(MAKE) clean -C $(SOLVERS_DIR)/mapleCOMSPS
	$(MAKE) clean -C $(SOLVERS_DIR)/minisat
	$(MAKE) clean -C $(SOLVERS_DIR)/glucose
	$(MAKE) clean -C $(SOLVERS_DIR)/yalsat
	$(MAKE) clean -C $(SOLVERS_DIR)/tassat
	$(MAKE) -C $(SOLVERS_DIR)/lingeling clean

clean: cleanpainless cleansolvers

cleanall: clean
	$(MAKE) -C $(M4RI_DIR) clean

.PHONY: clean
