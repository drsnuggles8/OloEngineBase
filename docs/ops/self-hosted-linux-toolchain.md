# Self-hosted Linux runners: the compiler is provisioned, selected by absolute path, and never installed by a job

Issue [#1015](https://github.com/drsnuggles8/OloEngineBase/issues/1015), item A. Applies to
every job that calls [`setup-linux-build`](../../.github/actions/setup-linux-build/action.yml)
on the `olo-ci` runners: the three Linux sanitizer jobs, `vulkan-off`, `steam-stub`, and the
GPU-under-sanitizer nightly.

## The rule

1. The hosted arm builds with clang-19 from apt.llvm.org (19.1.7). The self-hosted arm builds
   with the same major version when the box has it: EPEL's `clang19` / `compiler-rt19` /
   `lld19` install under `/usr/lib64/llvm19/bin`, off `PATH`, and the action selects them by
   absolute path. `-fuse-ld=lld` resolves the `ld.lld` beside the compiler before it searches
   `PATH`, so the linker follows without a second setting.
2. When the pin is missing, the action prints a `::warning` annotation naming the install
   command and builds with the system clang (21 on Rocky 10). It does not fail. A self-hosted
   step verifies and never installs (the convention at the top of the action), and a hard
   failure here would block every same-repo PR's sanitizer jobs on one maintainer running
   `dnf`.
3. Every configure log starts with one line, `toolchain: <compiler> (<version>) / <python>`.
   When a self-hosted job fails and its hosted twin does not, read that line before reading
   anything else.

Install the pin with `sudo bash scripts/setup-olo-ci-host.sh` (see
[self-hosted-host-hygiene.md](self-hosted-host-hygiene.md)).

## Why the pin is optional: the version skew was never the cause

#1010 measured the sanitizer jobs on the box, saw 215 failures against 1 hosted, and blamed
half of them on "clang 21 against gcc-toolset-15's libstdc++": two UBSan reports inside the
standard library, `downcast of misaligned address 0x000036f1c2f9` in `hashtable_policy.h`
and `execution reached an unreachable program point` in `stl_vector.h`. Two independent checks
say otherwise:

- A ~200-line probe of `std::unordered_map` and `std::vector` (insert, erase, rehash, node
  handles, 8- and 16-byte-aligned payloads, ~200k elements) built on the box with the exact
  `cmake/Sanitizers.cmake` UBSan flags is clean at `-O0` through `-O3`, under
  `-fsanitize=undefined`, with `_GLIBCXX_ASSERTIONS`, and under both libstdc++ pairings the box
  offers (`--gcc-install-dir` for gcc 14, and the default gcc-toolset-15). There is nothing to
  fix in the pairing. The probe is kept at `/tmp/ubsan-probe/` on the box while it lasts.
- The two reports have one stack: `~ShaderLibrary` → `~Ref<Shader>` → `~OpenGLShader` →
  `ShaderRegistry::UnregisterShader` walking a freed `unordered_map`. That is a destruction-order
  use-after-free in the engine (`ShaderRegistry::Get()` was a plain function-local static,
  destroyed before the static `ShaderLibrary` whose shaders unregister from it), fixed on
  #1015's branch by making the registry never-destructed (the ADR 0004 shape). Any compiler
  reproduces it given a GL context; the hosted runner has none, so it never saw it.

The other 212 failures were one bug too: SSBO binding slots 80..83 exceed Mesa's
`GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS` of 80 (NVIDIA reports 96), so `DDGI_ProbeMaintain`
fails to compile in `Renderer3D::Init` and asserts in a Debug build. Also fixed on that branch.

So the pin buys exact parity with the hosted gate, which is worth one `dnf install` on a box
that now runs every PR's sanitizer jobs; it does not buy a working build, which the box already
had. Two compiler versions in the fleet remain coverage, as long as the log says which one ran.

## Facts about the box that decided this

- `clang-libs` on Rocky 10 hard-requires `gcc-toolset-15-gcc-c++`, and
  `/etc/clang/x86_64-redhat-linux-gnu-clang++.cfg` pins the system clang to that libstdc++.
  Both pairings link the same runtime `/lib64/libstdc++.so.6.0.33` (GLIBCXX_3.4.33); the
  toolset ships only `libstdc++_nonshared.a`.
- `std::forward_like` compiles against both libstdc++ 14 and 15, so the reason the hosted arm
  pins a newer standard library (ubuntu-24.04's default lacks it) does not apply here.
- No LLVM 19.1.x release tarball exists for `x86_64-linux-gnu-ubuntu-22.04`; the only x86_64
  binary is `LLVM-19.1.7-Linux-X64.tar.xz` (1.65 GB). EPEL's package is the same 19.1.7 and one
  command, so the tarball route was not taken.
- `libc++` is not installed; `-stdlib=libc++` fails at `<cstdint>`.
