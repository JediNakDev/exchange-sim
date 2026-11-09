# Exchange simulator build.
#
#   make                    library objects, applications and test tools (MODE=release)
#   make test               build + run the full test suite
#   make test MODE=debug    same, with AddressSanitizer + UBSan
#   make test MODE=tsan     same, with ThreadSanitizer
#   make check              test in release and debug (sanitizer) modes
#   make apps MODE=perf    optimised API and worker binaries (-O3 -march=native -flto)
#   make bench              exsim protocol benchmarks (MODE=bench)
#   make legacy             standalone book tests, tools and examples
#   make legacy-bench       standalone book benchmark suite
#   make clean
#
# Sources are discovered automatically:
#   lib/src/**/*.cpp        reusable library implementation
#   src/<daemon>/*.cpp      daemon entry points, linked separately
#   test/{exsim,framework,lob}/**/*.cpp and test/main.cpp: run_tests
#   test/tools/<name>/*.cpp validation and diagnostic CLIs
#   legacy/examples/*.cpp   opt-in via make examples
#   test/benchmark/**/*.cpp build/bench/benchmark

.DEFAULT_GOAL := all
MODE ?= release

CXX ?= c++
STD := -std=c++20
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS := -I lib/include -I . -MMD -MP
# The production MatchingEngine is linked into run_tests, so the differential,
# scenario, preflight and allocation tests also run against it by default.
CPPFLAGS += -DEXSIM_TEST_PRODUCTION
LDLIBS := -lpthread

# PostgreSQL is a control-plane dependency, optional for standalone simulator builds.
WITH_POSTGRES ?= 0
PG_CONFIG ?= pg_config
ifeq ($(WITH_POSTGRES),1)
  CPPFLAGS += -DEXSIM_WITH_POSTGRES -I$(shell $(PG_CONFIG) --includedir)
  LDLIBS += -L$(shell $(PG_CONFIG) --libdir) -lpq
endif

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

LIB_SRCS := $(shell find lib/src -name '*.cpp' | sort)
TEST_SRCS := test/main.cpp $(shell find test/exsim test/framework test/lob -name '*.cpp' | sort)
EXAMPLE_SRCS := $(sort $(wildcard legacy/examples/*.cpp))
DAEMON_NAMES := exsim_apid exsim_traderd exsim_exchanged
DAEMON_BINS := $(addprefix $(BUILD)/,$(DAEMON_NAMES))

TEST_TOOL_NAMES := $(sort $(patsubst test/tools/%/,%,$(dir $(wildcard test/tools/*/*.cpp))))
TEST_TOOL_SRCS := $(wildcard test/tools/*/*.cpp)
TEST_TOOL_BINS := $(addprefix $(BUILD)/,$(TEST_TOOL_NAMES))


ifeq ($(shell uname -s),Darwin)
  BENCHMARK_PREFIX := $(shell brew --prefix google-benchmark 2>/dev/null || echo /opt/homebrew/opt/google-benchmark)
  BENCH_CPPFLAGS := -I$(BENCHMARK_PREFIX)/include
  BENCH_LDFLAGS := -L$(BENCHMARK_PREFIX)/lib
endif

# Standalone LOB applications and sharding code are opt-in.
LEGACY_SRCS := $(shell find legacy/src -name '*.cpp' 2>/dev/null | sort)
LEGACY_OBJS := $(call obj,$(LEGACY_SRCS))
LEGACY_TEST_SRCS := $(wildcard legacy/test/lob/*.cpp)
LEGACY_TOOL_SRCS := $(wildcard legacy/test/tools/*/*.cpp)
LEGACY_BENCH_SRCS := $(shell find legacy/test/benchmark -name '*.cpp' 2>/dev/null | sort)
LEGACY_TEST_BIN := $(BUILD)/legacy/run_tests
LEGACY_TOOL_BIN := $(BUILD)/legacy/itch_replay
LEGACY_BENCH_BIN := build/bench/legacy/benchmark

# Quote arbitrary configuration text as one shell argument.
shell_quote = '$(subst ','"'"',$(1))'
# Parse-time content stamps change timestamps only when the configuration changes.
# Separate modes retain independent configurations. Source removal forces relink.
$(shell mkdir -p $(BUILD))
$(shell printf '%s\n' $(call shell_quote,$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BENCH_CPPFLAGS)) > $(BUILD)/compile.tmp)
$(shell cmp -s $(BUILD)/compile.tmp $(BUILD)/compile.config || cp $(BUILD)/compile.tmp $(BUILD)/compile.config)
$(shell printf '%s\n' $(call shell_quote,$(CXX) $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) $(BENCH_LDFLAGS)) > $(BUILD)/link.tmp)
$(shell cmp -s $(BUILD)/link.tmp $(BUILD)/link.config || cp $(BUILD)/link.tmp $(BUILD)/link.config)
$(shell printf '%s\n' $(call shell_quote,$(LIB_SRCS) $(TEST_SRCS) $(EXAMPLE_SRCS) $(TEST_TOOL_SRCS) $(wildcard src/*/*.cpp) $(LEGACY_SRCS) $(LEGACY_TEST_SRCS) $(LEGACY_TOOL_SRCS) $(LEGACY_BENCH_SRCS) $(shell find test/benchmark -name '*.cpp' | sort)) > $(BUILD)/sources.tmp)
$(shell cmp -s $(BUILD)/sources.tmp $(BUILD)/sources.config || cp $(BUILD)/sources.tmp $(BUILD)/sources.config)

LIB_OBJS := $(call obj,$(LIB_SRCS))
BOOK_OBJS := $(call obj,lib/src/order_book.cpp)
TEST_BIN := $(BUILD)/run_tests
EXAMPLE_BINS := $(patsubst legacy/examples/%.cpp,$(BUILD)/legacy/examples/%,$(EXAMPLE_SRCS))

.PHONY: all lib test check apps test-tools examples bench bench-run clean itch-replay

$(TEST_BIN) $(TEST_TOOL_BINS) $(EXAMPLE_BINS): $(BUILD)/link.config $(BUILD)/sources.config

all: $(DAEMON_BINS) $(BUILD)/exsim_api $(TEST_BIN) $(TEST_TOOL_BINS)

lib: $(LIB_OBJS)

$(BUILD)/obj/%.o: %.cpp $(BUILD)/compile.config
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_BIN): | $(BUILD)/exsim_traderd $(BUILD)/exsim_exchanged

$(TEST_BIN): $(call obj,$(TEST_SRCS)) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(LDLIBS)

define DAEMON_RULE
$(BUILD)/$(1): $(call obj,$(sort $(wildcard src/$(2)/*.cpp))) $(LIB_OBJS) $(BUILD)/link.config $(BUILD)/sources.config
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$(LDFLAGS) $$(filter %.o,$$^) -o $$@ $$(LDLIBS)
endef
$(eval $(call DAEMON_RULE,exsim_apid,api))
$(eval $(call DAEMON_RULE,exsim_traderd,trader))
$(eval $(call DAEMON_RULE,exsim_exchanged,exchange))

$(BUILD)/exsim_apid: | $(BUILD)/exsim_exchanged $(BUILD)/exsim_traderd
$(BUILD)/exsim_research: | $(BUILD)/exsim_traderd

$(BUILD)/exsim_api: $(BUILD)/exsim_apid $(BUILD)/exsim_exchanged $(BUILD)/exsim_traderd
	cp $(BUILD)/exsim_apid $@

define TEST_TOOL_RULE
$(BUILD)/$(1): $(call obj,$(sort $(wildcard test/tools/$(1)/*.cpp))) $(LIB_OBJS)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(CXXFLAGS) $$(LDFLAGS) $$(filter %.o,$$^) -o $$@ $$(LDLIBS)
endef
$(foreach t,$(TEST_TOOL_NAMES),$(eval $(call TEST_TOOL_RULE,$(t))))

test-tools: $(TEST_TOOL_BINS)

$(BUILD)/legacy/examples/%: $(BUILD)/obj/legacy/examples/%.o $(BOOK_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN) $(FILTER)

check:
	$(MAKE) MODE=release test
	$(MAKE) MODE=debug test

apps: $(DAEMON_BINS) $(BUILD)/exsim_api

examples: $(EXAMPLE_BINS)

# Back-compat alias for the 1.4.0 README.
itch-replay:
	$(MAKE) MODE=perf build/perf/legacy/itch_replay

# ---------------------------------------------------------------------------
# Google Benchmark suite (Homebrew on macOS, system install on Linux).
# ---------------------------------------------------------------------------
BENCH_SRCS := $(shell find test/benchmark -name '*.cpp' | sort)
BENCH_BIN := build/bench/benchmark

ifeq ($(MODE),bench)
$(BENCH_BIN): $(BUILD)/link.config $(BUILD)/sources.config
$(BENCH_BIN): CPPFLAGS += -I test/benchmark $(BENCH_CPPFLAGS)
$(BENCH_BIN): $(call obj,$(BENCH_SRCS)) $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)

$(BUILD)/obj/test/benchmark/%.o: CPPFLAGS += -I test/benchmark $(BENCH_CPPFLAGS)
endif

bench:
	$(MAKE) MODE=bench $(BENCH_BIN)

bench-run: bench
	./$(BENCH_BIN) $(ARGS)

clean:
	rm -rf build

.PHONY: legacy legacy-test legacy-tools legacy-bench
legacy: legacy-test legacy-tools examples
legacy-tools: $(LEGACY_TOOL_BIN)
legacy-test: $(LEGACY_TEST_BIN)
	./$(LEGACY_TEST_BIN) $(FILTER)

$(BUILD)/obj/legacy/%.o: CPPFLAGS += -I legacy/include

$(LEGACY_TEST_BIN) $(LEGACY_TOOL_BIN) $(EXAMPLE_BINS): $(BUILD)/link.config $(BUILD)/sources.config
$(LEGACY_TEST_BIN): $(call obj,test/main.cpp test/framework/alloc_counter.cpp $(LEGACY_TEST_SRCS)) $(LEGACY_OBJS) $(BOOK_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(LDLIBS)

$(LEGACY_TOOL_BIN): $(call obj,$(LEGACY_TOOL_SRCS)) $(BOOK_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(LDLIBS)

ifeq ($(MODE),bench)
$(LEGACY_BENCH_BIN): $(BUILD)/link.config $(BUILD)/sources.config
$(LEGACY_BENCH_BIN): $(call obj,$(LEGACY_BENCH_SRCS)) $(LEGACY_OBJS) $(BOOK_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(filter %.o,$^) -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)
$(BUILD)/obj/legacy/test/benchmark/%.o: CPPFLAGS += -I legacy/test/benchmark $(BENCH_CPPFLAGS)
endif

legacy-bench:
	$(MAKE) MODE=bench $(LEGACY_BENCH_BIN)

ALL_OBJS := $(LIB_OBJS) $(call obj,$(TEST_SRCS) $(EXAMPLE_SRCS) $(BENCH_SRCS) $(TEST_TOOL_SRCS) $(wildcard src/*/*.cpp) $(LEGACY_SRCS) $(LEGACY_TEST_SRCS) $(LEGACY_TOOL_SRCS) $(LEGACY_BENCH_SRCS))
-include $(ALL_OBJS:.o=.d)
