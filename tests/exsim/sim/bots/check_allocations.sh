#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../.."
mkdir -p tests/exsim/sim/bots/logs
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow -Iinclude -I. \
    tests/main.cpp tests/exsim/sim/bots/allocation_check.cc src/exsim/sim/bots/config.cpp \
    -o tests/exsim/sim/bots/logs/allocation_check
exec tests/exsim/sim/bots/logs/allocation_check
