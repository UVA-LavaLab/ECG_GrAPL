CC  := $(shell which gcc-9 || which gcc)
CXX := $(shell which g++-9 || which g++)
PYTHON ?= python3
PARALLEL ?= $(shell grep -c ^processor /proc/cpuinfo)
RABBIT_ENABLE ?= 0

BENCH_DIR := bench
INC_DIR := $(BENCH_DIR)/include
BIN_DIR := $(BENCH_DIR)/bin
BIN_SIM_DIR := $(BENCH_DIR)/bin_sim
BIN_GEM5_DIR := $(BENCH_DIR)/bin_gem5
BIN_SNIPER_DIR := $(BENCH_DIR)/bin_sniper

INCLUDE_GAPBS := $(INC_DIR)/external/gapbs
INCLUDE_GRAPHBREW := $(INC_DIR)/graphbrew
INCLUDE_EXTERNAL := $(INC_DIR)/external
INCLUDE_CACHE := $(INC_DIR)/cache_sim

INCLUDES := -I$(INCLUDE_GAPBS) -I$(INCLUDE_GRAPHBREW) \
	-I$(INCLUDE_EXTERNAL) -I$(INC_DIR)

CXXFLAGS := -std=c++17 -O3 -Wall -fopenmp -g -DNDEBUG -m64 -march=native \
	-DTYPE=float -DMAX_THREADS=$(PARALLEL) -DREPEAT_METHOD=1
LDLIBS :=

ifeq ($(RABBIT_ENABLE),1)
CXXFLAGS += -DRABBIT_ENABLE -mcx16 -Wno-deprecated-declarations \
	-Wno-parentheses -Wno-unused-local-typedefs
LDLIBS += -ltcmalloc_minimal -lnuma
endif

DEP_GAPBS := $(wildcard $(INCLUDE_GAPBS)/*.h)
DEP_GRAPH := $(wildcard $(INCLUDE_GRAPHBREW)/reorder/*.h) \
	$(wildcard $(INCLUDE_GRAPHBREW)/partition/*.h)
DEP_EXTERNAL := $(wildcard $(INCLUDE_EXTERNAL)/rabbit/*.hpp) \
	$(wildcard $(INCLUDE_EXTERNAL)/gorder/*.h) \
	$(wildcard $(INCLUDE_EXTERNAL)/corder/*.h) \
	$(wildcard $(INCLUDE_EXTERNAL)/leiden/*.hxx)
DEP_CACHE := $(wildcard $(INCLUDE_CACHE)/*.h) \
	$(INC_DIR)/ecg_victim_policy.h \
	$(INC_DIR)/ecg_mode6_builder.h \
	$(INC_DIR)/ecg_reuse_plan_builder.h
# Shared ECG metadata/transport headers. These were tracked by NO build rule, so
# editing ecg_metadata.h or gem5_harness.h left every kernel binary stale while
# make reported success -- a trap that silently produced binaries missing the
# change under test.
DEP_ECG := $(wildcard $(INC_DIR)/ecg_*.h) \
	$(wildcard $(INC_DIR)/gem5_sim/*.h) \
	$(wildcard $(INC_DIR)/sniper_sim/*.h)

KERNELS_SIM := pr pr_spmv bfs bc cc cc_sv sssp tc ecg_preprocess \
	reuse_plan_sidecar test_ecg_reuse_plan test_ecg_reuse_plan32
KERNELS_GEM5 := pr pr_spmv bfs sssp cc cc_sv bc tc
KERNELS_SNIPER := sg_kernel pr bfs sssp bc cc cc_sv \
	pr_kernel_smoke bfs_kernel_smoke sssp_kernel_smoke hello_roi

.PHONY: all artifact converter all-sim all-gem5 all-sniper \
	sim-% gem5-% gem5-m5ops-% gem5-riscv-m5ops-% sniper-% \
	setup-gem5 setup-gem5-guest-tools setup-sniper test verify clean clean-sim clean-gem5-bin \
	clean-sniper-bin generate-wiki-figures check-wiki-figures help

all: all-sim

artifact: all-sim
	@echo "cache_sim ECG artifact built"

help:
	@echo "ECG artifact targets:"
	@echo "  make all-sim                  Build cache_sim kernels"
	@echo "  make converter                Build .el -> .sg converter"
	@echo "  make setup-gem5               Install/build gem5 overlays"
	@echo "  make setup-gem5-guest-tools   Install receipt/snapshot tools"
	@echo "  make gem5-riscv-m5ops-pr      Build RISC-V ECG PageRank"
	@echo "  make setup-sniper             Install/build Sniper overlays"
	@echo "  make sniper-sg_kernel         Build canonical Sniper workload"
	@echo "  make generate-wiki-figures    Regenerate SVG and Draw.io figures"
	@echo "  make check-wiki-figures       Validate figure and mirror contract"
	@echo "  make test                     Run Python artifact tests"

$(BIN_DIR) $(BIN_SIM_DIR) $(BIN_GEM5_DIR) $(BIN_SNIPER_DIR):
	mkdir -p $@

converter: $(BIN_DIR)/converter

$(BIN_DIR)/converter: $(BENCH_DIR)/src/converter.cc $(DEP_GAPBS) \
	$(DEP_GRAPH) $(DEP_EXTERNAL) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LDLIBS) -o $@

$(BIN_SIM_DIR)/%: $(BENCH_DIR)/src_sim/%.cc $(DEP_GAPBS) \
	$(DEP_GRAPH) $(DEP_EXTERNAL) $(DEP_CACHE) $(DEP_ECG) | $(BIN_SIM_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(INCLUDE_CACHE) $< $(LDLIBS) -o $@

.PRECIOUS: $(BIN_SIM_DIR)/%

sim-%: $(BIN_SIM_DIR)/%
	@echo "Built cache_sim kernel: $<"

all-sim: $(addprefix $(BIN_SIM_DIR)/,$(KERNELS_SIM))
	@echo "Built all cache_sim ECG kernels"

GEM5_SIM_DIR := $(INC_DIR)/gem5_sim
GEM5_DIR := $(GEM5_SIM_DIR)/gem5
GEM5_M5_DIR := $(GEM5_DIR)/util/m5
GEM5_M5_LIB := $(GEM5_M5_DIR)/build/x86/out/libm5.a
GEM5_M5_RISCV_LIB := $(GEM5_M5_DIR)/build/riscv/out/libm5.a
RISCV_CXX ?= riscv64-linux-gnu-g++
RISCV_CROSS_COMPILE ?= riscv64-linux-gnu-
GEM5_GUEST_RECEIPT := scripts/experiments/ecg/gem5_guest_receipt.py
GEM5_RISCV_BUILD_CONFIG := $(BIN_GEM5_DIR)/.riscv_build_config
GEM5_GUEST_STRACE ?= /usr/bin/strace
GEM5_GUEST_PROOT ?= $(INC_DIR)/gem5_sim/.tools/proot
GEM5_GUEST_PROOT_LOADER ?= /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
GEM5_GUEST_PROOT_LIBC ?= /usr/lib/x86_64-linux-gnu/libc.so.6
GEM5_GUEST_PROOT_TALLOC ?= /usr/lib/x86_64-linux-gnu/libtalloc.so.2
GEM5_GUEST_FUSEPY ?= $(INC_DIR)/gem5_sim/.tools/fusepy.py
GEM5_GUEST_LIBFUSE ?= $(INC_DIR)/gem5_sim/.tools/libfuse.so.2
GEM5_GUEST_FUSERMOUNT ?= /usr/bin/fusermount3
GEM5_GUEST_PYTHON ?= /usr/bin/python3.12
GEM5_GUEST_CLEAN_ENV := env -i PATH=/usr/bin:/bin HOME=/tmp TMPDIR=/tmp \
	LC_ALL=C LANG=C
RISCV_CXX_RESOLVED := $(shell command -v $(RISCV_CXX) 2>/dev/null)

CXXFLAGS_GEM5 := -std=c++17 -O3 -Wall -g -DNDEBUG -DNO_M5OPS -fopenmp
CXXFLAGS_GEM5_M5OPS := $(filter-out -DNO_M5OPS,$(CXXFLAGS_GEM5)) \
	-I$(GEM5_DIR)/include
CXXFLAGS_GEM5_RISCV := $(CXXFLAGS_GEM5_M5OPS) -funswitch-loops -static -mno-relax
RISCV_GUEST_BINARIES := $(addsuffix _riscv_m5ops,$(addprefix \
	$(BIN_GEM5_DIR)/,$(KERNELS_GEM5)))
RISCV_GUEST_DEPFILES := $(addsuffix .d,$(RISCV_GUEST_BINARIES))
RISCV_GUEST_RECEIPTS := $(addsuffix .build.json,$(RISCV_GUEST_BINARIES))

.PHONY: FORCE_GEM5_RISCV_CONFIG
.PRECIOUS: $(RISCV_GUEST_BINARIES) $(RISCV_GUEST_DEPFILES) \
	$(RISCV_GUEST_RECEIPTS)

FORCE_GEM5_RISCV_CONFIG:

$(GEM5_RISCV_BUILD_CONFIG): FORCE_GEM5_RISCV_CONFIG | $(BIN_GEM5_DIR)
	@{ \
		printf '%s\n' "RISCV_CXX=$(RISCV_CXX)"; \
		printf '%s\n' "RISCV_CXX_RESOLVED=$(RISCV_CXX_RESOLVED)"; \
		printf '%s\n' "CXXFLAGS_GEM5_RISCV=$(CXXFLAGS_GEM5_RISCV)"; \
		printf '%s\n' "INCLUDES=$(INCLUDES)"; \
		printf '%s\n' "STRACE=$(GEM5_GUEST_STRACE)"; \
		printf '%s\n' "PROOT=$(abspath $(GEM5_GUEST_PROOT))"; \
		printf '%s\n' "PROOT_LOADER=$(GEM5_GUEST_PROOT_LOADER)"; \
		printf '%s\n' "PROOT_LIBC=$(GEM5_GUEST_PROOT_LIBC)"; \
		printf '%s\n' "PROOT_TALLOC=$(GEM5_GUEST_PROOT_TALLOC)"; \
		printf '%s\n' "FUSEPY=$(abspath $(GEM5_GUEST_FUSEPY))"; \
		printf '%s\n' "LIBFUSE=$(abspath $(GEM5_GUEST_LIBFUSE))"; \
		printf '%s\n' "FUSERMOUNT=$(GEM5_GUEST_FUSERMOUNT)"; \
		printf '%s\n' "PYTHON=$(GEM5_GUEST_PYTHON)"; \
		printf '%s\n' "PATH=/usr/bin:/bin"; \
		printf '%s\n' "COMPILER_PATH="; \
		printf '%s\n' "GCC_EXEC_PREFIX="; \
		printf '%s\n' "LIBRARY_PATH="; \
		printf '%s\n' "CPATH="; \
		printf '%s\n' "CPLUS_INCLUDE_PATH="; \
		printf '%s\n' "HOME=/tmp"; \
		printf '%s\n' "TMPDIR=/tmp"; \
		printf '%s\n' "LC_ALL=C"; \
		printf '%s\n' "LANG=C"; \
	} > $@.tmp
	@if test -f $@ && cmp -s $@.tmp $@; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
	fi

GEM5_DEP_GOALS := $(filter \
	gem5-% all-gem5 \
	$(BIN_GEM5_DIR)/%_riscv_m5ops \
	$(BIN_GEM5_DIR)/%_riscv_m5ops.d \
	$(BIN_GEM5_DIR)/%_riscv_m5ops.build.json,$(MAKECMDGOALS))
ifneq ($(GEM5_DEP_GOALS),)
-include $(wildcard $(BIN_GEM5_DIR)/*_riscv_m5ops.d)
endif

$(BIN_GEM5_DIR)/%: $(BENCH_DIR)/src_gem5/%.cc $(DEP_GAPBS) \
	$(DEP_GRAPH) $(DEP_EXTERNAL) $(DEP_ECG) | $(BIN_GEM5_DIR)
	$(CXX) $(CXXFLAGS_GEM5) $(INCLUDES) $< $(LDLIBS) -o $@

$(GEM5_M5_LIB):
	cd $(GEM5_M5_DIR) && scons -j$(PARALLEL) build/x86/out/m5

$(GEM5_M5_RISCV_LIB):
	cd $(GEM5_M5_DIR) && scons -j$(PARALLEL) build/riscv/out/m5 \
		riscv.CROSS_COMPILE=$(RISCV_CROSS_COMPILE)

$(BIN_GEM5_DIR)/%_m5ops: $(BENCH_DIR)/src_gem5/%.cc $(DEP_GAPBS) \
	$(DEP_GRAPH) $(DEP_EXTERNAL) $(DEP_ECG) $(GEM5_M5_LIB) | $(BIN_GEM5_DIR)
	$(CXX) $(CXXFLAGS_GEM5_M5OPS) $(INCLUDES) $< \
		$(GEM5_M5_LIB) $(LDLIBS) -o $@

.PRECIOUS: $(BIN_GEM5_DIR)/%_m5ops

$(BIN_GEM5_DIR)/%_riscv_m5ops \
$(BIN_GEM5_DIR)/%_riscv_m5ops.d \
$(BIN_GEM5_DIR)/%_riscv_m5ops.build.json &: \
	$(BENCH_DIR)/src_gem5/%.cc $(GEM5_M5_RISCV_LIB) \
	$(RISCV_CXX_RESOLVED) $(GEM5_GUEST_STRACE) \
	$(GEM5_GUEST_PROOT) $(GEM5_GUEST_PROOT_LOADER) \
	$(GEM5_GUEST_PROOT_LIBC) $(GEM5_GUEST_PROOT_TALLOC) \
	$(GEM5_GUEST_FUSEPY) $(GEM5_GUEST_LIBFUSE) \
	$(GEM5_GUEST_FUSERMOUNT) $(GEM5_GUEST_PYTHON) \
	$(GEM5_RISCV_BUILD_CONFIG) | \
	$(BIN_GEM5_DIR)
	$(GEM5_GUEST_CLEAN_ENV) $(GEM5_GUEST_PYTHON) -I \
		$(GEM5_GUEST_RECEIPT) build \
		--receipt $(BIN_GEM5_DIR)/$*_riscv_m5ops.build.json \
		--binary $(BIN_GEM5_DIR)/$*_riscv_m5ops \
		--depfile $(BIN_GEM5_DIR)/$*_riscv_m5ops.d \
		--compiler "$(RISCV_CXX)" --flags "$(CXXFLAGS_GEM5_RISCV)" \
		--includes "$(INCLUDES)" --source $< \
		--link-input $(GEM5_M5_RISCV_LIB) \
		--build-config $(GEM5_RISCV_BUILD_CONFIG) \
		--make-target $(BIN_GEM5_DIR)/$*_riscv_m5ops

gem5-%: $(BIN_GEM5_DIR)/%
	@echo "Built gem5 kernel: $<"

gem5-m5ops-%: $(BIN_GEM5_DIR)/%_m5ops
	@echo "Built gem5 m5ops kernel: $<"

gem5-riscv-m5ops-%: $(BIN_GEM5_DIR)/%_riscv_m5ops \
	$(BIN_GEM5_DIR)/%_riscv_m5ops.d \
	$(BIN_GEM5_DIR)/%_riscv_m5ops.build.json
	@echo "Built RISC-V gem5 kernel: $<"

all-gem5: $(addprefix $(BIN_GEM5_DIR)/,$(KERNELS_GEM5))
	@echo "Built all native gem5 ECG kernels"

SNIPER_DIR := $(INC_DIR)/sniper_sim/snipersim
SNIPER_INCLUDE := $(SNIPER_DIR)/include
CXXFLAGS_SNIPER := -std=c++17 -O2 -Wall -g -DNDEBUG -fopenmp \
	-I$(INC_DIR) -I$(SNIPER_INCLUDE)

$(BIN_SNIPER_DIR)/%: $(BENCH_DIR)/src_sniper/%.cc $(DEP_GAPBS) \
	$(DEP_GRAPH) $(DEP_EXTERNAL) $(DEP_ECG) | $(BIN_SNIPER_DIR)
	$(CXX) $(CXXFLAGS_SNIPER) $(INCLUDES) $< $(LDLIBS) -o $@

sniper-%: $(BIN_SNIPER_DIR)/%
	@echo "Built Sniper kernel: $<"

all-sniper: $(addprefix $(BIN_SNIPER_DIR)/,$(KERNELS_SNIPER))
	@echo "Built all Sniper ECG kernels"

setup-gem5:
	$(PYTHON) scripts/setup_gem5.py --isa X86 RISCV --jobs $(PARALLEL)

setup-gem5-guest-tools:
	$(PYTHON) scripts/setup_gem5_guest_tools.py

setup-sniper:
	$(PYTHON) scripts/setup_sniper.py --jobs $(PARALLEL) --apply-overlays

test:
	pytest -q scripts/test

generate-wiki-figures:
	$(PYTHON) scripts/docs/generate_ecg_figures.py

check-wiki-figures:
	$(PYTHON) scripts/docs/generate_ecg_figures.py --check
	$(PYTHON) scripts/docs/check_wiki_figures.py

verify:
	$(PYTHON) scripts/experiments/ecg/verify/equiv_kernels.py \
		--gem5 --sniper --kernels pr bfs --reuse-plan-depth 2

clean-sim:
	rm -rf $(BIN_SIM_DIR)

clean-gem5-bin:
	rm -rf $(BIN_GEM5_DIR)

clean-sniper-bin:
	rm -rf $(BIN_SNIPER_DIR)

.PHONY: clean clean-all clean-sim clean-gem5-bin clean-sniper-bin

clean: clean-sim clean-gem5-bin clean-sniper-bin

# Local research notes are intentionally outside build cleanup.
clean-all: clean
	rm -rf build m5out sim.out sniper.out .pytest_cache
	@echo "Generated build/simulator outputs removed; research/ and results/ preserved."
