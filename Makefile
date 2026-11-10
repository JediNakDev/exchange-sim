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
else
$(error MODE must be release or debug)
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
