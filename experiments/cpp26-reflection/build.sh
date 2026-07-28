#!/usr/bin/env bash
# ============================================================================
#  Build a C++26-reflection experiment TU with the in-tree GCC 16.1.
#
#  GCC 16.1 with -freflection is REQUIRED (mainline Clang 21 / MSVC have neither
#  reflection nor annotations). The compiler here is an uninstalled in-tree build,
#  so the invocation is fiddly — override the two paths for your machine:
#     GCC16_BUILD  in-tree GCC 16.1 build dir (contains gcc/xg++)   [machine-specific]
#     OLO_ROOT     OloEngineBase repo root (for the --engine tests)
#
#  Usage:
#     ./build.sh <file.cpp>              self-contained test (glm + yaml-cpp only)
#     ./build.sh --engine <file.cpp>     real-engine test (PCH + all vendors + UUID.cpp)
#     ./build.sh --engine --syntax <f>   compile-only proof (static_asserts; no link)
# ============================================================================
set -euo pipefail
GCC16_BUILD="${GCC16_BUILD:-/home/obueker/gcc-16-build}"
OLO_ROOT="${OLO_ROOT:-/home/obueker/projects/OloEngineBase}"

MODE=self; SYNTAX=0
while [[ "${1:-}" == --* ]]; do
  case "$1" in --engine) MODE=engine;; --syntax) SYNTAX=1;; *) echo "unknown flag $1"; exit 2;; esac; shift
done
SRC="${1:?usage: build.sh [--engine] [--syntax] file.cpp}"
OUT="${SRC%.cpp}"

XG="$GCC16_BUILD/gcc"
INCS=$("$GCC16_BUILD/x86_64-pc-linux-gnu/libstdc++-v3/scripts/testsuite_flags" --build-includes)
LIBSTDCPP="$GCC16_BUILD/x86_64-pc-linux-gnu/libstdc++-v3/src/.libs"
V="$OLO_ROOT/OloEngine/vendor"
YAMLA="$V/yaml-cpp-build/Debug/libyaml-cpp.a"
STD="-std=c++26 -freflection -O0 -w"

if [[ "$MODE" == self ]]; then
  INC="-I. -I$V/glm-src -I$V/yaml-cpp-src/include"
  "$XG/xg++" -B"$XG" $INCS $INC -L"$LIBSTDCPP" -Wl,-rpath,"$LIBSTDCPP" $STD "$SRC" "$YAMLA" -o "$OUT"
else
  # every vendor include root Components.h transitively needs
  INC="-I. -I$OLO_ROOT/OloEngine/src -I$V/glm-src -I$V/entt-src/single_include -I$V/spdlog-src/include \
       -I$V/box2d-src/include -I$V/yaml-cpp-src/include -I$V/joltphysics-src -I$V/miniaudio-src -I$V/stb_image-src \
       -I$V/choc-src -I$V/glad-build/include -I$V/nlohmann_json-src/include -I$V/assimp-src/include \
       -I$V/assimp-build/include -I$V/tracy-src/public"
  PCH="-include $OLO_ROOT/OloEngine/src/OloEnginePCH.h"   # SceneCamera.h etc. use f32 without including Base.h
  EXTRA="-fpermissive -fconstexpr-depth=4000"            # accept MSVC/Clang-isms GCC 16 rejects; deep CanSerialize
  if [[ "$SYNTAX" == 1 ]]; then
    "$XG/xg++" -B"$XG" $INCS $INC $PCH $STD $EXTRA -fsyntax-only "$SRC" && echo "[syntax OK] $SRC"
    exit 0
  fi
  "$XG/xg++" -B"$XG" $INCS $INC $PCH -L"$LIBSTDCPP" -Wl,-rpath,"$LIBSTDCPP" $STD $EXTRA \
    "$SRC" "$OLO_ROOT/OloEngine/src/OloEngine/Core/UUID.cpp" "$YAMLA" -o "$OUT"
fi
echo "[built $OUT]  run with:  LD_LIBRARY_PATH=$LIBSTDCPP ./$OUT"
