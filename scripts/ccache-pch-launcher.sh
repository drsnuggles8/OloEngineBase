#!/bin/sh
# ccache-pch-launcher.sh <ccache> <compiler> <args...>
#
# Compiler launcher that runs every compile through ccache unchanged, EXCEPT a compile
# that PRODUCES a precompiled header, which it runs with ccache's base_dir cleared.
# Wired up by cmake/CompilerCache.cmake; issue #1460 has the failure it prevents.
#
# Why only the PCH, and why base_dir. base_dir makes two checkouts at different
# absolute roots hash alike, which is how the two olo-ci runner slots share objects.
# That is safe for an object, because -ffile-prefix-map makes its bytes independent of
# the root. It is NOT safe for a .pch: clang records the absolute path of every header
# it read, and a consumer that includes one of those headers through its own root gets
# a second copy of each `#pragma once` header -- "redefinition of 'ArraySize'", with the
# previous definition in the OTHER slot's checkout.
#
# The key collision is the non-obvious part. ccache never serves a PCH from preprocessor
# mode, only from direct mode, and each slot's direct-mode manifest entry is private to
# it (it hashes cmake_pch.hxx, whose #include line is absolute). But the RESULT key is
# computed from the preprocessed output, which base_dir makes root-independent, so both
# slots store their .pch under ONE result key and each overwrites the other's. Slot 1's
# own manifest entry then leads it to slot 2's .pch. Clearing base_dir for this one
# compile puts the absolute root back into both keys, so each slot keeps its own PCH.
#
# Nothing else is lost: a PCH consumer's key includes the .pch file's hash, which already
# differs per slot, so PCH consumers never shared across slots; non-PCH objects still do.
#
# `CCACHE_BASEDIR=` (set, empty) rather than `unset`: an empty environment value also
# overrides a base_dir set in ccache.conf; unset would let the file's value through.

ccache="$1"
shift

for arg in "$@"; do
    case "$arg" in
        # clang's PCH action (CMake passes it as `-Xclang -emit-pch`), and GCC's
        # header-as-input language, which is how GCC produces a .gch.
        -emit-pch | c-header | c++-header | -xc-header | -xc++-header)
            CCACHE_BASEDIR= exec "$ccache" "$@"
            ;;
    esac
done

exec "$ccache" "$@"
