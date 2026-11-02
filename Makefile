# Exchange simulator build.
#
#   make                    library objects, tests, tools, examples (MODE=release)
#   make test               build + run the full test suite
#   make test MODE=debug    same, with AddressSanitizer + UBSan
#   make test MODE=tsan     same, with ThreadSanitizer
#   make check              test in release and debug (sanitizer) modes
#   make tools MODE=perf    optimised tool binaries (-O3 -march=native -flto)
#   make bench              Google Benchmark suite (MODE=bench)
#   make clean
#
# Sources are discovered automatically:
#   src/**/*.cpp            linked into every binary
#   tests/**/*.cpp          one test binary: build/<mode>/run_tests
#   tools/<name>/*.cpp      one binary per directory: build/<mode>/<name>
#   examples/<name>.cpp     one binary per file: build/<mode>/examples/<name>
#   benchmark/**/*.cpp      build/bench/benchmark

MODE ?= release

CXX ?= c++
STD := -std=c++20
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS := -I include -I . -MMD -MP
LDLIBS := -lpthread

ifeq ($(MODE),release)
  OPT := -O2 -g
else ifeq ($(MODE),debug)
  OPT := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
  LDOPT := -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  OPT := -O1 -g -fsanitize=thread
  LDOPT := -fsanitize=thread
else ifeq ($(MODE),perf)
  OPT := -O3 -march=native -DNDEBUG -flto -fno-omit-frame-pointer
  LDOPT := -flto
else ifeq ($(MODE),bench)
  OPT := -O3 -march=native -mtune=native -DNDEBUG -flto -fno-omit-frame-pointer -DLOB_DETERMINISTIC_POOL
  LDOPT := -flto
else
  $(error unknown MODE '$(MODE)' (release|debug|tsan|perf|bench))
endif

CXXFLAGS := $(STD) $(OPT) $(WARNINGS) $(EXTRA_CXXFLAGS)
LDFLAGS := $(LDOPT) $(EXTRA_LDFLAGS)

BUILD := build/$(MODE)
obj = $(patsubst %.cpp,$(BUILD)/obj/%.o,$(1))

LIB_SRCS := $(shell find src -name '*.cpp' | sort)
TEST_SRCS := $(shell find tests -name '*.cpp' | sort)
EXAMPLE_SRCS := $(sort $(wildcard examples/*.cpp))
TOOL_NAMES := $(sort $(patsubst tools/%/,%,$(dir $(wildcard tools/*/*.cpp))))

LIB_OBJS := $(call obj,$(LIB_SRCS))
TEST_BIN := $(BUILD)/run_tests
TOOL_BINS := $(addprefix $(BUILD)/,$(TOOL_NAMES))
EXAMPLE_BINS := $(patsubst examples/%.cpp,$(BUILD)/examples/%,$(EXAMPLE_SRCS))

.PHONY: all lib test check tools examples bench bench-run clean itch-replay

all: $(TEST_BIN) $(TOOL_BINS) $(EXAMPLE_BINS)

lib: $(LIB_OBJS)

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_BIN): $(call obj,$(TEST_SRCS)) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

define TOOL_RULE
$(BUILD)/$(1): $(call obj,$(sort $(wildcard tools/$(1)/*.cpp))) $(LIB_OBJS)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$(LDFLAGS) $$^ -o $$@ $$(LDLIBS)
endef
$(foreach t,$(TOOL_NAMES),$(eval $(call TOOL_RULE,$(t))))

$(BUILD)/examples/%: $(BUILD)/obj/examples/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN) $(FILTER)

check:
	$(MAKE) MODE=release test
	$(MAKE) MODE=debug test

tools: $(TOOL_BINS)

examples: $(EXAMPLE_BINS)

# Back-compat alias for the 1.4.0 README.
itch-replay:
	$(MAKE) MODE=perf build/perf/itch_replay

# ---------------------------------------------------------------------------
# Google Benchmark suite (Homebrew on macOS, system install on Linux).
# ---------------------------------------------------------------------------
ifeq ($(shell uname -s),Darwin)
  BENCHMARK_PREFIX := $(shell brew --prefix google-benchmark 2>/dev/null || echo /opt/homebrew/opt/google-benchmark)
  BENCH_CPPFLAGS := -I$(BENCHMARK_PREFIX)/include
  BENCH_LDFLAGS := -L$(BENCHMARK_PREFIX)/lib
endif

BENCH_SRCS := $(shell find benchmark -name '*.cpp' | sort)
BENCH_BIN := build/bench/benchmark

ifeq ($(MODE),bench)
$(BENCH_BIN): CPPFLAGS += -I benchmark $(BENCH_CPPFLAGS)
$(BENCH_BIN): $(call obj,$(BENCH_SRCS)) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)

$(BUILD)/obj/benchmark/%.o: CPPFLAGS += -I benchmark $(BENCH_CPPFLAGS)
endif

bench:
	$(MAKE) MODE=bench $(BENCH_BIN)

bench-run: bench
	./$(BENCH_BIN) $(ARGS)

clean:
	rm -rf build

ALL_OBJS := $(LIB_OBJS) $(call obj,$(TEST_SRCS) $(EXAMPLE_SRCS) $(BENCH_SRCS) $(wildcard tools/*/*.cpp))
-include $(ALL_OBJS:.o=.d)
