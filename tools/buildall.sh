#!/usr/bin/env bash
# Build and test every compiler lane available on this machine, then run
# pledge-check on the produced binary. A lane whose compiler is absent is
# SKIPped loudly (CI still covers it); a lane that fails fails the script.
#
# Windows: run from a shell with MSYS2 mingw64 on PATH (g++, cmake, ninja).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

overall=0

run_lane() {
    local lane="$1" cxx="$2" stdlib="${3:-}"
    if ! command -v "$cxx" > /dev/null 2>&1; then
        echo "[$lane] SKIP — $cxx not on PATH (CI covers this lane)"
        return 0
    fi
    local -a flags=()
    if [ -n "$stdlib" ]; then
        # A standard library this machine has not got is not a failure: the
        # lane that needs it still runs in CI.
        if ! echo 'int main() { return 0; }' \
            | "$cxx" -x c++ -std=c++23 "-stdlib=$stdlib" -fsyntax-only - > /dev/null 2>&1; then
            echo "[$lane] SKIP — $cxx has no $stdlib here (CI covers this lane)"
            return 0
        fi
        flags=(-DCMAKE_CXX_FLAGS="-stdlib=$stdlib" -DCMAKE_EXE_LINKER_FLAGS="-stdlib=$stdlib")
    fi
    echo "[$lane] configure + build + test ($cxx${stdlib:+, $stdlib})"
    if cmake -S . -B "build-$lane" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER="$cxx" "${flags[@]}" > "build-$lane.configure.log" 2>&1 \
        && cmake --build "build-$lane" > "build-$lane.build.log" 2>&1 \
        && ctest --test-dir "build-$lane" --output-on-failure > "build-$lane.test.log" 2>&1; then
        mv -f "build-$lane".*.log "build-$lane/" 2> /dev/null
        echo "[$lane] PASS"
    else
        echo "[$lane] FAIL — logs:"
        tail -n 25 "build-$lane.build.log" 2> /dev/null || tail -n 25 "build-$lane.configure.log"
        overall=1
    fi
}

run_lane gcc g++
run_lane clang clang++
# macOS builds against libc++, and the two standard libraries do not declare
# the same names in the same headers: a name one of them pulls in changes
# which overload a call finds, and a type one of them spells differently
# changes what an arithmetic expression means. Both have broken the macOS
# lane from code that built and passed here, so libc++ is a lane of its own.
run_lane libcxx clang++ libc++

binary="build-gcc/sashfold"
[ -f "$binary.exe" ] && binary="$binary.exe"
if [ -f "$binary" ]; then
    bash tools/pledge-check.sh "$binary" || overall=1
else
    echo "pledge-check: no gcc-lane binary to inspect"
fi
bash tools/egress-check.sh || overall=1

if [ "$overall" -eq 0 ]; then
    echo "buildall: all lanes green"
else
    echo "buildall: FAILURES above"
fi
exit "$overall"
