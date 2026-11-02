#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
mkdir -p tests/exsim/net/logs
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow -Iinclude \
    tests/exsim/net/allocation_check.cc src/exsim/net/*.cpp \
    -o tests/exsim/net/logs/allocation_check
exec tests/exsim/net/logs/allocation_check
