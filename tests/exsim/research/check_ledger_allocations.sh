#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p tests/exsim/research/logs
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow -Iinclude \
    tests/exsim/research/ledger_allocation_check.cc src/exsim/research/position_ledger.cpp \
    -o tests/exsim/research/logs/ledger_allocation_check
exec tests/exsim/research/logs/ledger_allocation_check
