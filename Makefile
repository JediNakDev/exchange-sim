.DEFAULT_GOAL := all
MODE ?= release
CXX ?= c++
BUILD := build/$(MODE)
CPPFLAGS := -I lib/include -I . -MMD -MP
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wshadow $(EXTRA_CXXFLAGS)
ifeq ($(MODE),debug)
CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else ifeq ($(MODE),release)
CXXFLAGS += -O2 -g
else ifeq ($(MODE),perf)
CXXFLAGS += -O3 -march=native -DNDEBUG -flto -fno-omit-frame-pointer
LDFLAGS += -flto
else ifeq ($(MODE),bench)
CXXFLAGS += -O3 -march=native -mtune=native -DNDEBUG -flto -fno-omit-frame-pointer -DLOB_DETERMINISTIC_POOL
LDFLAGS += -flto
else
$(error MODE must be release, debug, perf or bench)
endif
LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS := -lpthread
TEST_LIB_SRCS := lib/src/exsim/md/book_builder.cpp lib/src/exsim/net/tcp_client.cpp lib/src/exsim/server/admin.cpp lib/src/exsim/server/core/admin_text.cpp
LIB_SRCS := $(filter-out $(TEST_LIB_SRCS),$(shell find lib/src -name '*.cpp' | sort))
obj = $(patsubst %.cpp,$(BUILD)/obj/%.o,$(1))
LIB_OBJS := $(call obj,$(LIB_SRCS))
TEST_LIB_OBJS := $(call obj,$(TEST_LIB_SRCS))
# Unit coverage stays local to codecs, gateways, matching and core policies.
UNIT_SRCS := $(shell find test/lob test/framework test/exsim/engine test/exsim/protocol test/exsim/gateway test/exsim/md test/exsim/server/core -name '*.cpp' | sort)
UNIT_SRCS := $(filter-out %udp_loopback_tests.cpp,$(UNIT_SRCS))
UNIT_SRCS += test/exsim/api/api_json_tests.cpp test/exsim/server/config_tests.cpp test/exsim/server/config_review_tests.cpp test/exsim/server/admin_tests.cpp test/exsim/sim/packet_faults_tests.cpp test/exsim/foundation/contract_tests.cpp
INTEGRATION_SRCS := $(shell find test/exsim/net -name '*.cpp' | sort) test/exsim/api/http_tests.cpp test/exsim/md/udp_loopback_tests.cpp test/framework/alloc_counter.cpp
.PHONY: all apps test unit integration clean
all: apps
apps: $(BUILD)/exsim_api $(BUILD)/exsim_exchanged
$(BUILD)/obj/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/exsim_exchanged: $(call obj,src/exchange/main.cpp) $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/exsim_api: $(call obj,src/api/main.cpp) $(LIB_OBJS) | $(BUILD)/exsim_exchanged
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/unit_tests: $(call obj,test/main.cpp $(UNIT_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests $(FILTER)
$(BUILD)/integration_tests: $(call obj,test/main.cpp $(INTEGRATION_SRCS)) $(LIB_OBJS) $(TEST_LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)
integration: apps $(BUILD)/integration_tests
	./$(BUILD)/integration_tests $(FILTER)
	EXSIM_BUILD=$(BUILD) python3 -B test/integration/exchange_api_test.py
test: unit integration
clean:
	rm -rf $(BUILD)
-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)

# Standalone LOB tests and benchmarks restored from release 0.1.2.
BOOK_OBJS := $(call obj,lib/src/order_book.cpp)
LOB_TEST_SRCS := $(sort $(wildcard tests/*.cpp))
LOB_BENCH_SRCS := $(sort $(wildcard benchmark/*.cpp benchmark/cases/*.cpp benchmark/support/*.cpp))
LOB_BENCH_BIN := build/bench/lob_benchmark

ifeq ($(shell uname -s),Darwin)
BENCHMARK_PREFIX := $(shell brew --prefix google-benchmark 2>/dev/null || echo /opt/homebrew/opt/google-benchmark)
BENCH_CPPFLAGS := -I$(BENCHMARK_PREFIX)/include
BENCH_LDFLAGS := -L$(BENCHMARK_PREFIX)/lib
endif

.PHONY: lob-test benchmark benchmark-run benchmark-filter itch-replay
$(BUILD)/obj/tests/%.o: CXXFLAGS += -UNDEBUG
$(BUILD)/lob_tests: $(call obj,$(LOB_TEST_SRCS)) $(BOOK_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
lob-test: $(BUILD)/lob_tests
	./$(BUILD)/lob_tests

$(BUILD)/itch_replay: $(call obj,tools/itch_replay/replay_main.cpp) $(BOOK_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)
itch-replay:
	$(MAKE) MODE=perf build/perf/itch_replay

ifeq ($(MODE),bench)
$(BUILD)/obj/benchmark/%.o: CPPFLAGS += -I benchmark $(BENCH_CPPFLAGS)
$(LOB_BENCH_BIN): $(call obj,$(LOB_BENCH_SRCS)) $(BOOK_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(BENCH_LDFLAGS) -lbenchmark $(LDLIBS)
endif
benchmark:
	$(MAKE) MODE=bench $(LOB_BENCH_BIN)
benchmark-run: benchmark
	./$(LOB_BENCH_BIN) $(ARGS)
benchmark-filter: benchmark
	./$(LOB_BENCH_BIN) --benchmark_filter=$(FILTER) $(ARGS)
