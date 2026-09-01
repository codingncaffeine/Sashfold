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
    local lane="$1" cxx="$2"
    if ! command -v "$cxx" > /dev/null 2>&1; then
        echo "[$lane] SKIP — $cxx not on PATH (CI covers this lane)"
        return 0
    fi
    echo "[$lane] configure + build + test ($cxx)"
    if cmake -S . -B "build-$lane" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_COMPILER="$cxx" > "build-$lane.configure.log" 2>&1 \
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
