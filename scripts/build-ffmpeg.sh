#!/usr/bin/env bash
# =============================================================================
# build-ffmpeg.sh — build a minimal, decode-only FFmpeg (shared libs) from source
# for the OloEngine FFmpeg video backend. Cross-platform:
#
#   * Windows (MSYS/MINGW bash): builds with the MSVC toolchain (--toolchain=msvc)
#     so the import libs link natively into the MSVC-built engine. Needs a Visual
#     Studio install (located via vswhere), nasm, and gmake (Strawberry Perl).
#   * Linux / other Unix: native gcc/clang build. Needs nasm + make.
#
# Driven by CMake's ExternalProject (cmake/ffmpeg.cmake), but standalone-runnable:
#   build-ffmpeg.sh <ffmpeg-src-dir> <install-prefix> [build|configure-only]
# =============================================================================
set -euo pipefail

SRC="${1:?ffmpeg source dir required}"
PREFIX="${2:?install prefix required}"
MODE="${3:-build}"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OLO_OS=windows ;;
    *)                    OLO_OS=unix ;;
esac

# On Windows, CMake's ExternalProject passes Windows paths; normalise to MINGW form.
if [ "$OLO_OS" = windows ]; then
    SRC="$(cygpath -u "$SRC" 2>/dev/null || echo "$SRC")"
    PREFIX="$(cygpath -u "$PREFIX" 2>/dev/null || echo "$PREFIX")"
fi

# GNU make: `gmake` on Windows (Strawberry); `make` on Linux. FFmpeg needs GNU make.
MAKE="$(command -v gmake || command -v make)"

# --- Windows only: import the MSVC build environment into this shell. ---
if [ "$OLO_OS" = windows ]; then
    VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    VSPATH="$("$VSWHERE" -latest -property installationPath)"
    VCVARS="$VSPATH/VC/Auxiliary/Build/vcvars64.bat"
    [ -f "$VCVARS" ] || { echo "vcvars64.bat not found at $VCVARS" >&2; exit 1; }

    # Dump the environment vcvars64 sets up via a temp .bat (CRLF; avoids the nested
    # bash->cmd quoting that breaks an inline `cmd //c "call ... && set"`).
    ENVTMP="$(mktemp)"
    DUMPBAT="$(mktemp --suffix=.bat)"
    printf '@echo off\r\ncall "%s"\r\nset\r\n' "$(cygpath -w "$VCVARS")" > "$DUMPBAT"
    cmd.exe //c "$(cygpath -w "$DUMPBAT")" > "$ENVTMP" 2>/dev/null
    rm -f "$DUMPBAT"

    # cl reads INCLUDE / LIB / LIBPATH as Windows-path env vars directly.
    export INCLUDE="$(grep -i '^INCLUDE=' "$ENVTMP" | head -1 | cut -d= -f2- | tr -d '\r')"
    export LIB="$(grep -i '^LIB=' "$ENVTMP" | head -1 | cut -d= -f2- | tr -d '\r')"
    export LIBPATH="$(grep -i '^LIBPATH=' "$ENVTMP" | head -1 | cut -d= -f2- | tr -d '\r')"

    # Prepend every dir vcvars added to PATH (MINGW form) so cl/link/lib/rc resolve —
    # and MSVC's link.exe shadows MSYS coreutils /usr/bin/link, which would otherwise
    # break the --toolchain=msvc linker step.
    VCPATH="$(grep -i '^PATH=' "$ENVTMP" | head -1 | cut -d= -f2- | tr -d '\r')"
    MINGWADD="$(echo "$VCPATH" | tr ';' '\n' | while read -r d; do [ -n "$d" ] && cygpath -u "$d" 2>/dev/null || true; done | paste -sd: -)"
    export PATH="$MINGWADD:$PATH"
    rm -f "$ENVTMP"

    command -v cl >/dev/null || { echo "cl not on PATH after vcvars import" >&2; exit 1; }
    echo "Using cl: $(command -v cl)"
fi

command -v nasm >/dev/null || { echo "nasm not found (install: apt-get install nasm / choco install nasm)" >&2; exit 1; }
echo "Using make: $MAKE"
echo "Using nasm: $(command -v nasm)"

cd "$SRC"

# Selective decode set shared by both platforms. A full decoder set overflows Windows'
# ~32KB command line when avcodec's export .def is generated, so we enable just what the
# engine needs for MP4/MOV/MKV/H.264/HEVC/VP9 + MPEG. ac3+eac3 decoders satisfy the
# ac3/eac3 parser table deps. swscale (YUV->RGBA) + swresample (audio->stereo f32) stay on.
CODEC_FLAGS="\
    --enable-shared --disable-static --disable-programs --disable-doc --disable-network \
    --disable-everything \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    --enable-protocol=file \
    --enable-demuxer=mov,matroska,m4v,h264,hevc,mpegvideo,mpegps,avi,aac,mp3 \
    --enable-parser=h264,hevc,aac,ac3,mpeg4video,mpegvideo,mpegaudio,vp9,vp8 \
    --enable-decoder=h264,hevc,aac,ac3,eac3,mp3,mp2,mpeg1video,mpeg2video,mpeg4,vp8,vp9,pcm_s16le"

# Skip the (slow) configure when the tree is already configured, unless asked to redo it.
if [ -f ffbuild/config.mak ] && [ "$MODE" != "configure-only" ] && [ "${OLO_FFMPEG_RECONFIGURE:-0}" != "1" ]; then
    echo "=== already configured (ffbuild/config.mak present) — skipping configure ==="
else
    if [ "$OLO_OS" = windows ]; then
        # shellcheck disable=SC2086
        ./configure --prefix="$(cygpath -w "$PREFIX")" --toolchain=msvc --target-os=win64 --arch=x86_64 $CODEC_FLAGS 2>&1 | tail -40
    else
        # shellcheck disable=SC2086
        ./configure --prefix="$PREFIX" $CODEC_FLAGS 2>&1 | tail -40
    fi
    # A reconfigure must start from clean objects (the Windows dep no-op below defeats
    # make's incremental detection; on Linux a codec-set change wants a clean tree too).
    "$MAKE" clean >/dev/null 2>&1 || true
fi

if [ "$MODE" = "configure-only" ]; then
    echo "=== configure-only: done ==="
    exit 0
fi

if [ "$OLO_OS" = windows ]; then
    # Neutralise FFmpeg's `-showIncludes` dependency awk: Strawberry's native-Windows gmake
    # eats a backslash from the recipe before any shell sees it, mangling `gsub(/\\/,"/")`
    # into an unterminated regex. The .d files only matter for incremental rebuilds; for a
    # one-shot build, replace the dep command with a no-op that writes an empty .d.
    sed -i -E 's#^(CCDEP|CXXDEP|ASDEP|HOSTCCDEP)=.*#\1=: > $(@:.o=.d)#' ffbuild/config.mak
    echo "=== patched ffbuild/config.mak dep commands to no-op (Windows gmake) ==="
fi

"$MAKE" -j"$(nproc)" 2>&1 | tail -25
"$MAKE" install 2>&1 | tail -15

if [ "$OLO_OS" = windows ]; then
    # FFmpeg's MSVC `make install` ships the DLLs + headers + .def but leaves the import
    # .lib in the build tree. Copy them where ffmpeg.cmake / the linker expect them.
    mkdir -p "$PREFIX/lib"
    for _l in avcodec avformat avutil swscale swresample; do
        cp -f "$SRC/lib${_l}/${_l}.lib" "$PREFIX/lib/${_l}.lib" 2>/dev/null \
            && echo "  copied import lib: ${_l}.lib" || echo "  WARNING: ${_l}.lib not found in build tree"
    done
fi

echo "=== FFmpeg build + install complete: $PREFIX ==="
