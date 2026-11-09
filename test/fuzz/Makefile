# Coverage-guided fuzzing (libFuzzer + AddressSanitizer + UBSan).
#
# Needs clang with libFuzzer (Linux; Apple clang has no libFuzzer). The
# docker/Dockerfile image has everything:
#
#   docker build -t exsim-dev docker
#   docker run --rm -v "$PWD":/src exsim-dev make -C /src/test/fuzz
#
#   make -C test/fuzz                       build every target into build/fuzz/
#   make -C test/fuzz seeds                 regenerate the deterministic seeds into build/fuzz/seeds/
#   make -C test/fuzz run TARGET=itch50     campaign: starts from test/fuzz/corpus/itch50, writes
#                                      new inputs to build/fuzz/campaign/itch50 and crashes
#                                      to build/fuzz/artifacts/itch50
#   make -C test/fuzz merge TARGET=itch50   minimize test/fuzz/corpus/itch50 + the campaign corpus
#                                      back into test/fuzz/corpus/itch50
#   make -C test/fuzz replay TARGET=itch50 INPUT=path/to/input
#
# Campaign knobs: SECONDS (per job, default 300), JOBS and WORKERS (default 2;
# the machine is shared, keep them small), FUZZ_FLAGS for extra libFuzzer flags.
#
# Every target body is a header in targets/ that the normal test suite also
# replays (test/exsim/fuzz_regression), so `make test` re-checks the committed
# corpora on every run without libFuzzer.

REPOSITORY_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
FUZZ_DIRECTORY := $(REPOSITORY_ROOT)/test/fuzz
BUILD_DIRECTORY := $(REPOSITORY_ROOT)/build/fuzz

CXX := clang++
SEED_CXX ?= c++
FUZZ_SANITIZERS := -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS := -std=c++20 -O1 -g -fno-omit-frame-pointer $(FUZZ_SANITIZERS) \
            -Wall -Wextra -Wpedantic -Wshadow $(EXTRA_CXXFLAGS)
CPPFLAGS := -I $(REPOSITORY_ROOT)/lib/include -I $(REPOSITORY_ROOT) -MMD -MP

TARGETS := ouch42_inbound ouch42_outbound itch50 soupbintcp moldudp64 fix44_message fix44_stream \
           wire_text_fields stream_buffers admin_command server_config price_text journal_reader \
           scenario_dsl bot_options sbe_order_entry

LIBRARY_SOURCES := $(shell cd $(REPOSITORY_ROOT) && find lib/src -name '*.cpp' | sort)
LIBRARY_OBJECTS := $(patsubst %.cpp,$(BUILD_DIRECTORY)/obj/%.o,$(LIBRARY_SOURCES))
TARGET_BINARIES := $(addprefix $(BUILD_DIRECTORY)/,$(TARGETS))

SECONDS ?= 300
JOBS ?= 2
WORKERS ?= 2
FUZZ_FLAGS ?=

.PHONY: all seeds run merge replay clean check-target
.DEFAULT_GOAL := all

all: $(TARGET_BINARIES)

# Library sources get coverage instrumentation but no fuzzer main.
$(BUILD_DIRECTORY)/obj/%.o: $(REPOSITORY_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsanitize=fuzzer-no-link -c $< -o $@

$(BUILD_DIRECTORY)/entry/%.o: $(FUZZ_DIRECTORY)/libfuzzer_entry.cpp $(FUZZ_DIRECTORY)/targets/%.hpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsanitize=fuzzer-no-link \
	    -include $(FUZZ_DIRECTORY)/targets/$*.hpp -DFUZZ_TARGET_FUNCTION=exsim::fuzz::fuzz_$* -c $< -o $@

$(BUILD_DIRECTORY)/%: $(BUILD_DIRECTORY)/entry/%.o $(LIBRARY_OBJECTS)
	$(CXX) $(CXXFLAGS) -fsanitize=fuzzer $^ -o $@ -lpthread

$(BUILD_DIRECTORY)/seed_generator: $(FUZZ_DIRECTORY)/seed_generator.cpp $(LIBRARY_SOURCES:%=$(REPOSITORY_ROOT)/%)
	@mkdir -p $(dir $@)
	$(SEED_CXX) -std=c++20 -O1 -I $(REPOSITORY_ROOT)/lib/include -I $(REPOSITORY_ROOT) \
	    $(FUZZ_DIRECTORY)/seed_generator.cpp $(LIBRARY_SOURCES:%=$(REPOSITORY_ROOT)/%) -o $@ -lpthread

seeds: $(BUILD_DIRECTORY)/seed_generator
	rm -rf $(BUILD_DIRECTORY)/seeds
	$(BUILD_DIRECTORY)/seed_generator $(REPOSITORY_ROOT) $(BUILD_DIRECTORY)/seeds

check-target:
	@test -n "$(filter $(TARGET),$(TARGETS))" || { echo "TARGET must be one of: $(TARGETS)"; exit 2; }

# -jobs/-workers run JOBS jobs, WORKERS at a time, each for at most SECONDS.
# test/fuzz/corpus/<target> is read-only input here; new inputs go to the campaign
# directory and are folded back with `make merge`.
run: check-target $(BUILD_DIRECTORY)/$(TARGET)
	@mkdir -p $(BUILD_DIRECTORY)/campaign/$(TARGET) $(BUILD_DIRECTORY)/artifacts/$(TARGET) $(BUILD_DIRECTORY)/logs/$(TARGET)
	cd $(BUILD_DIRECTORY)/logs/$(TARGET) && $(BUILD_DIRECTORY)/$(TARGET) \
	    -jobs=$(JOBS) -workers=$(WORKERS) -max_total_time=$(SECONDS) -use_value_profile=1 -timeout=10 \
	    -rss_limit_mb=2048 -print_final_stats=1 \
	    -artifact_prefix=$(BUILD_DIRECTORY)/artifacts/$(TARGET)/ $(FUZZ_FLAGS) \
	    $(BUILD_DIRECTORY)/campaign/$(TARGET) $(FUZZ_DIRECTORY)/corpus/$(TARGET)

merge: check-target $(BUILD_DIRECTORY)/$(TARGET)
	rm -rf $(BUILD_DIRECTORY)/merged/$(TARGET)
	@mkdir -p $(BUILD_DIRECTORY)/merged/$(TARGET) $(BUILD_DIRECTORY)/campaign/$(TARGET)
	$(BUILD_DIRECTORY)/$(TARGET) -merge=1 -timeout=10 -rss_limit_mb=2048 \
	    $(BUILD_DIRECTORY)/merged/$(TARGET) $(FUZZ_DIRECTORY)/corpus/$(TARGET) $(BUILD_DIRECTORY)/campaign/$(TARGET)
	rm -rf $(FUZZ_DIRECTORY)/corpus/$(TARGET)
	mv $(BUILD_DIRECTORY)/merged/$(TARGET) $(FUZZ_DIRECTORY)/corpus/$(TARGET)

replay: check-target $(BUILD_DIRECTORY)/$(TARGET)
	@test -n "$(INPUT)" || { echo "INPUT=<file> is required"; exit 2; }
	$(BUILD_DIRECTORY)/$(TARGET) -runs=1 $(INPUT)

clean:
	rm -rf $(BUILD_DIRECTORY)

-include $(shell find $(BUILD_DIRECTORY) -name '*.d' 2>/dev/null)
