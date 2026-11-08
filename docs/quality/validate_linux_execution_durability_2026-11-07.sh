#!/usr/bin/env bash
# Run inside a disposable exsim-dev container capped at 2 CPUs.
# Host copies a git archive to /tmp/exsim-source.tar before starting it.
# Outputs live in /validation; no checkout or installed service is mounted.
set -euo pipefail

mkdir -p /validation/source /validation/logs
exec > >(tee /validation/logs/driver.log) 2>&1
trap 'status=$?; echo "driver_exit=$status"' EXIT
tar -xf /tmp/exsim-source.tar -C /validation/source
cd /validation/source

uname -a
cat /etc/os-release
g++ --version
clang++ --version
make --version
stat -f -c 'source_filesystem=%T' .
cat /sys/fs/cgroup/cpu.max
sha256sum /tmp/exsim-source.tar

# This existing development image lacks Python. Install only the required
# interpreter/stdlib in this throwaway container, without building an image.
if ! command -v python3 >/dev/null; then
    apt-get update > /validation/logs/python-install.log 2>&1
    apt-get install -y --no-install-recommends python3 >> /validation/logs/python-install.log 2>&1
fi
python3 --version
dpkg-query -W gcc g++ clang make python3 libstdc++6 libc6

if command -v rg >/dev/null; then
    mapfile -t api_sources < <(rg --files tests/exsim/api -g '*.cpp' | sort)
else
    mapfile -t api_sources < <(find tests/exsim/api -type f -name '*.cpp' | sort)
fi
test "${#api_sources[@]}" -eq 5
test_sources="tests/main.cpp ${api_sources[*]}"
printf '%s\n' tests/main.cpp "${api_sources[@]}" > /validation/logs/test-sources.txt
find . -type f -print0 | sort -z | xargs -0 sha256sum > /validation/logs/source-before.sha256

for compiler in gcc clang; do
    if [[ "$compiler" == gcc ]]; then cxx=g++; else cxx=clang++; fi
    build="/validation/build/$compiler"
    start=$SECONDS
    make -j2 MODE=release CXX="$cxx" BUILD="$build" EXTRA_CXXFLAGS=-Werror \
        TEST_SRCS="$test_sources" "$build/run_tests" > "/validation/logs/$compiler-test-build.log" 2>&1
    echo "$compiler test_build_seconds=$((SECONDS - start))"

    "$build/run_tests" --list > "/validation/logs/$compiler-test-list.log"
    total=$(wc -l < "/validation/logs/$compiler-test-list.log")
    durability=$(grep -c '^api_durability\.' "/validation/logs/$compiler-test-list.log")
    execution=$(grep -c '^execution_api\.' "/validation/logs/$compiler-test-list.log")
    test "$total" -eq 64
    test "$durability" -eq 17
    test "$execution" -eq 11
    echo "$compiler registered=$total durability=$durability execution=$execution"

    start=$SECONDS
    timeout 180 "$build/run_tests" > "/validation/logs/$compiler-tests.log" 2>&1
    grep -Fx '64/64 tests passed' "/validation/logs/$compiler-tests.log"
    echo "$compiler test_seconds=$((SECONDS - start))"

    set +e
    "$build/run_tests" __exsim_linux_unmatched_926e3cf2__ > "/validation/logs/$compiler-empty-filter.log" 2>&1
    empty_status=$?
    set -e
    echo "$compiler empty_filter_exit=$empty_status"
    test "$empty_status" -eq 2
    grep -Fx 'No tests matched filter: __exsim_linux_unmatched_926e3cf2__' "/validation/logs/$compiler-empty-filter.log"

    start=$SECONDS
    make -j2 MODE=release CXX="$cxx" BUILD="$build" EXTRA_CXXFLAGS=-Werror \
        TEST_SRCS="$test_sources" "$build/exsim_api" > "/validation/logs/$compiler-api-build.log" 2>&1
    test -x "$build/exsim_api"
    readelf -h "$build/run_tests" "$build/exsim_api"
    echo "$compiler api_build_seconds=$((SECONDS - start))"
    for log in "$compiler-test-build" "$compiler-api-build"; do
        if grep -En 'warning:|error:' "/validation/logs/$log.log"; then
            echo "Compiler diagnostic in $log"; exit 1
        fi
    done
    echo "$compiler compiler_warnings=0 compiler_errors=0"
done

start=$SECONDS
timeout 180 python3 -B scripts/deployment/test_research_delivery.py > /validation/logs/python-delivery.log 2>&1
grep -E '^Ran 40 tests in |^OK$' /validation/logs/python-delivery.log
grep -Eq '^Ran 40 tests in ' /validation/logs/python-delivery.log
grep -Fx 'OK' /validation/logs/python-delivery.log
echo "python delivery_seconds=$((SECONDS - start))"

find . -type f -print0 | sort -z | xargs -0 sha256sum > /validation/logs/source-after.sha256
cmp /validation/logs/source-before.sha256 /validation/logs/source-after.sha256
echo 'Source files unchanged; all focused Linux gates passed.'
