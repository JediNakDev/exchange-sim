#!/usr/bin/env bash
# Disposable container only: source archive at /tmp/exsim-source.tar; no mounts.
set -euo pipefail
mkdir -p /validation/source /validation/logs
exec > >(tee /validation/logs/driver.log) 2>&1
trap 'status=$?; echo "driver_exit=$status"' EXIT
tar -xf /tmp/exsim-source.tar -C /validation/source
cd /validation/source
uname -a
g++ --version
clang++ --version
cat /sys/fs/cgroup/cpu.max
sha256sum /tmp/exsim-source.tar /tmp/exsim-validation.sh
mapfile -t api_sources < <(find tests/exsim/api -type f -name '*.cpp' | sort)
test "${#api_sources[@]}" -eq 5
test_sources="tests/main.cpp ${api_sources[*]}"
printf '%s\n' tests/main.cpp "${api_sources[@]}" > /validation/logs/test-sources.txt
find . -type f -print0 | sort -z | xargs -0 sha256sum > /validation/logs/source-before.sha256
for compiler in gcc clang; do
    if [[ "$compiler" == gcc ]]; then cxx=g++; else cxx=clang++; fi
    build="/validation/build/$compiler"
    started=$SECONDS
    make -j2 MODE=release CXX="$cxx" BUILD="$build" EXTRA_CXXFLAGS=-Werror \
        TEST_SRCS="$test_sources" "$build/run_tests" > "/validation/logs/$compiler-test-build.log" 2>&1
    echo "$compiler test_build_seconds=$((SECONDS - started))"
    "$build/run_tests" --list > "/validation/logs/$compiler-test-list.log"
    test "$(wc -l < "/validation/logs/$compiler-test-list.log")" -eq 71
    test "$(grep -c '^api_io_failure\.' "/validation/logs/$compiler-test-list.log")" -eq 5
    test "$(grep -c '^api_cache_bounds\.' "/validation/logs/$compiler-test-list.log")" -eq 2
    started=$SECONDS
    timeout 60 "$build/run_tests" > "/validation/logs/$compiler-tests.log" 2>&1
    grep -Fx '71/71 tests passed' "/validation/logs/$compiler-tests.log"
    echo "$compiler test_seconds=$((SECONDS - started))"
    set +e
    "$build/run_tests" __exsim_recorder_no_match__ > "/validation/logs/$compiler-empty-filter.log" 2>&1
    empty_status=$?
    set -e
    test "$empty_status" -eq 2
    grep -Fx 'No tests matched filter: __exsim_recorder_no_match__' "/validation/logs/$compiler-empty-filter.log"
    echo "$compiler empty_filter_exit=$empty_status"
    started=$SECONDS
    make -j2 MODE=release CXX="$cxx" BUILD="$build" EXTRA_CXXFLAGS=-Werror \
        TEST_SRCS="$test_sources" "$build/exsim_api" > "/validation/logs/$compiler-api-build.log" 2>&1
    test -x "$build/exsim_api"
    readelf -h "$build/run_tests" "$build/exsim_api"
    echo "$compiler api_build_seconds=$((SECONDS - started))"
    if grep -En 'warning:|error:' "/validation/logs/$compiler-test-build.log" "/validation/logs/$compiler-api-build.log"; then
        echo 'Unexpected compiler diagnostics'; exit 1
    fi
    echo "$compiler compiler_warnings=0 compiler_errors=0"
done
find . -type f -print0 | sort -z | xargs -0 sha256sum > /validation/logs/source-after.sha256
cmp /validation/logs/source-before.sha256 /validation/logs/source-after.sha256
echo 'Source unchanged; focused Linux71 gates passed.'
