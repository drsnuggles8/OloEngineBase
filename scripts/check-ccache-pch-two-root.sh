#!/usr/bin/env bash
# check-ccache-pch-two-root.sh -- reproduce issue #1460 and prove the fix, in seconds.
#
# Two checkouts at different absolute roots (named like the two olo-ci runner slots) share
# one PRIVATE ccache directory with the same settings .github/actions/setup-linux-build
# exports: base_dir = each checkout's root, hash_dir off, the PCH sloppiness. A tiny CMake
# project with a real target_precompile_headers() PCH is built slot 1 -> slot 2 -> slot 1,
# the last time from a fresh build tree with one consumer edited, i.e. a PCH cache hit
# followed by a consumer miss. That is the sequence that failed CI run 36045750117.
#
#   control  plain ccache as the launcher. EXPECTED TO FAIL with a redefinition against the
#            OTHER slot's header -- that is what proves this check can see the bug. If it
#            passes, the installed ccache no longer collides PCH result keys and the check
#            has nothing to detect (a warning, not an error: see the end of the script).
#   fixed    scripts/ccache-pch-launcher.sh in front of ccache, as cmake/CompilerCache.cmake
#            wires it. MUST pass, its .pch must name only its own slot, and the non-PCH
#            object must still hit across slots (the sharing the base_dir setup exists for).
#
# Usage: scripts/check-ccache-pch-two-root.sh [ccache] [c++ compiler]
# Defaults: $OLO_COMPILER_CACHE_TOOL or `ccache`, and $CXX or `clang++`. Needs cmake.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
ccache_bin="${1:-${OLO_COMPILER_CACHE_TOOL:-$(command -v ccache || true)}}"
cxx="${2:-${CXX:-clang++}}"
[ -x "$ccache_bin" ] || { echo "::error::no ccache executable ('$ccache_bin')"; exit 1; }
command -v "$cxx" >/dev/null || { echo "::error::no C++ compiler ('$cxx')"; exit 1; }
echo "ccache: $("$ccache_bin" --version | head -1)"
echo "c++:    $("$cxx" --version | head -1)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/tmpl/src/Core"
cat > "$work/tmpl/src/Core/Base.h" <<'EOF'
#pragma once
template<typename T, int N>
constexpr int ArraySize(T (&)[N]) { return N; }
EOF
cat > "$work/tmpl/src/PCH.h" <<'EOF'
#pragma once
#include <vector>
#include "Core/Base.h"
EOF
printf '#include "PCH.h"\n' > "$work/tmpl/src/PCH.cpp"
printf '#include "PCH.h"\nint a() { int v[3]{}; return ArraySize(v); }\n' > "$work/tmpl/src/A.cpp"
printf '#include <vector>\nint plain() { return (int)std::vector<int>{1, 2}.size(); }\n' > "$work/tmpl/src/Plain.cpp"
cat > "$work/tmpl/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(pch_two_root CXX)
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/olo")
add_library(eng STATIC src/PCH.cpp src/A.cpp)
target_include_directories(eng PRIVATE src)
target_precompile_headers(eng PRIVATE src/PCH.h)
add_library(plain STATIC src/Plain.cpp)
EOF

# run_arm <name> <launcher list> -> sets step3_rc, step3_pch_other, plain_cross_hit
run_arm() {
    local name="$1" launcher="$2" slot step root log i=0
    local arm="$work/$name"
    mkdir -p "$arm"
    step3_rc=0 step3_pch_other=0 plain_cross_hit=no
    (
        # Private cache and logs: never touch the job's shared cache or its stats log.
        export CCACHE_DIR="$arm/cache" CCACHE_NOHASHDIR=1
        export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-pch_defines,time_macros,include_file_mtime,include_file_ctime}"
        unset CCACHE_STATSLOG CCACHE_DISABLE CCACHE_RECACHE CCACHE_READONLY
        for step in 1 2 1c; do
            i=$((i + 1))
            slot="${step%c}"
            root="$arm/actions-runner-ci-$slot/_work/Olo"
            if [ ! -d "$root" ]; then
                mkdir -p "$(dirname "$root")"
                cp -r "$work/tmpl" "$root"
            fi
            if [ "$step" != "$slot" ]; then
                rm -rf "$root/build"
                echo "int a$i() { return $i; }" >> "$root/src/A.cpp"
            fi
            export CCACHE_BASEDIR="$root" CCACHE_LOGFILE="$arm/ccache$i.log"
            cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Debug \
                "-DCMAKE_CXX_COMPILER=$cxx" "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher" > "$arm/configure$i.log" 2>&1 \
                || { echo "::error::[$name] configure failed"; cat "$arm/configure$i.log"; exit 2; }
            cmake --build "$root/build" -j 1 > "$arm/build$i.log" 2>&1
            echo "$?" > "$arm/rc$i"
        done
    ) || exit $?

    step3_rc=$(cat "$arm/rc3")
    # Which slot's paths the .pch in step 3 carries (clang writes them verbatim).
    local pch
    pch=$(find "$arm/actions-runner-ci-1/_work/Olo/build" -name 'cmake_pch.hxx.pch' | head -1)
    if [ -z "$pch" ]; then
        # A renamed PCH file must not turn the check below into a silent pass.
        echo "::error::[$name] no cmake_pch.hxx.pch in slot 1's tree -- cannot tell which slot's PCH it used"
        exit 1
    fi
    step3_pch_other=$(grep -a -c 'actions-runner-ci-2' "$pch")
    # The non-PCH object in step 2 (slot 2's first build) must be a hit on slot 1's entry.
    # One "Object file:" line and one final outcome line per invocation, in order.
    grep -E 'Object file: |Result: (direct_cache_hit|preprocessed_cache_hit|cache_miss)$' "$arm/ccache2.log" \
        | paste -d' ' - - | grep -qE 'Plain\.cpp\.o .*Result: (direct|preprocessed)_cache_hit$' \
        && plain_cross_hit=yes

    echo "[$name] slot1 -> slot2 -> slot1 (fresh tree, consumer edited): exit $step3_rc;" \
         "step-3 .pch lines naming slot 2: $step3_pch_other; non-PCH object hit across slots: $plain_cross_hit"
    grep -m2 -E 'error:|previous definition' "$arm/build3.log" | sed "s|$arm/||; s/^/    /"
}

failed=0

run_arm fixed "/bin/sh;$here/ccache-pch-launcher.sh;$ccache_bin"
if [ "$step3_rc" -ne 0 ] || [ "$step3_pch_other" -ne 0 ]; then
    echo "::error::ccache-pch-launcher.sh did not keep the PCH per checkout root: the slot-1 rebuild failed or was served slot 2's .pch (issue #1460)"
    failed=1
fi
if [ "$plain_cross_hit" != yes ]; then
    echo "::error::a non-PCH object no longer hits across checkout roots -- the launcher cost the cross-slot sharing it must keep"
    failed=1
fi

run_arm control "$ccache_bin"
if [ "$step3_rc" -eq 0 ]; then
    # Not an error: it means this ccache stopped colliding PCH result keys (an upstream
    # change), so the control cannot demonstrate the bug and the "fixed" arm above proves
    # less than it claims. Worth knowing before anyone relies on this check again.
    echo "::warning title=#1460 control did not reproduce::plain ccache ($("$ccache_bin" --version | head -1)) built slot1 -> slot2 -> slot1 cleanly, so this check can no longer demonstrate the PCH collision. Re-read scripts/ccache-pch-launcher.sh before trusting or removing it."
fi

exit "$failed"
