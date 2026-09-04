# Self-hosted Linux runners: the box builds with a pinned clang-23, and a job verifies but never installs

Issues [#1015](https://github.com/drsnuggles8/OloEngineBase/issues/1015) item A and
[#1036](https://github.com/drsnuggles8/OloEngineBase/issues/1036). Applies to every job that
calls [`setup-linux-build`](../../.github/actions/setup-linux-build/action.yml) on the `olo-ci`
runners: the three Linux sanitizer jobs, `vulkan-off`, `steam-stub`, and the GPU-under-sanitizer
nightly. All six take their compiler from that action's outputs, so the pin below moves all six,
not only the sanitizer arm the issue was filed about. Watch
[`gpu-sanitizers-amd.yml`](../../.github/workflows/gpu-sanitizers-amd.yml): its baseline is a
same-hardware comparison, so the first nightly after a toolchain change is a new baseline, not a
regression.

## The rule

1. **Both Linux arms build with clang-23.** Hosted takes it from apt.llvm.org; the box takes it
   from the official LLVM release tarball, unpacked at **`/opt/llvm-23.1.0`** by
   [`scripts/setup-olo-ci-host.sh`](../../scripts/setup-olo-ci-host.sh) and referenced by
   absolute path. The prefix is versioned and is **not** on the system `PATH`: Rocky's clang
   21.1.8 stays the default for everything else on the host, including another repository's
   runners.
2. **A package pin is impossible; that is not the same as no pin.** EPEL on Rocky 10 tops out at
   `clang20` (20.1.8) and ships no `clang22`/`clang23`. This doc previously concluded from that
   that the skew was permanent. It was not -- LLVM publishes prebuilt tarballs, so the skew was
   a choice.
3. **The pin is asserted, never assumed.** The pin this replaced named `/usr/lib64/llvm19`, a
   directory that never existed on the box, and nothing checked: every self-hosted sanitizer run
   silently took the fallback for as long as it stood. `setup-olo-ci-host.sh` re-verifies on
   every run -- compiler-rt complete, and a C++23 `std::stacktrace` program actually linking and
   running under asan, ubsan, tsan **and `-flto`**.
4. **It is a ladder, and every rung warns and carries on.** Four things are checked in order,
   and any one of them failing falls back to the system clang with a `::warning` naming
   `scripts/setup-olo-ci-host.sh`:

   | Rung | Fails when |
   |---|---|
   | the prefix is executable | nothing provisioned it |
   | it reports major 23 | a bump landed in the action before it landed on the box |
   | its `ld.lld --version` runs | unpacked without the ICU 70 libs beside it (next section) |
   | its compiler-rt is complete | `asan`/`tsan`/`ubsan` archives are not all beside it |

   The last one falls back **only if the system clang's runtimes are complete** — trading
   version parity for a second compiler that also cannot link would buy nothing. Version parity
   is what this job is *for*; a build that links is what it needs first.

   It must not FAIL: `asan.yml` routes every same-repo PR's sanitizer jobs here, and a hard
   failure would block all of them on one maintainer running one script. Read the warning as
   *"this job is no longer comparable with its hosted twin"*, not as *"this job is broken"*.
5. **The runtime check asks the compiler, not the distro.** A clang without `compiler-rt`
   configures fine and fails every sanitizer LINK on `cannot find libclang_rt.asan.a`. The
   directory comes from `clang++ -print-runtime-dir`, so the check follows the tarball's layout
   and the distro's alike.
6. **Every configure log still starts with one line**, `toolchain: <compiler> (<version>) /
   <python>`. Read it before anything else when a self-hosted job fails and its hosted twin does
   not -- it names an absolute path under `/opt` when the pin is in effect, and a bare `clang++`
   when it is not.

Provision with `sudo bash scripts/setup-olo-ci-host.sh` (idempotent; see
[self-hosted-host-hygiene.md](self-hosted-host-hygiene.md)). It needs ~16 GB free on the
filesystem holding `/opt` and takes roughly twenty minutes on a cold install — ten for the LLVM
tarball, the rest for the ICU build the next section explains.

## The linker is part of the pin, and getting it there took a detour

The tarball's `lld` does not start on Rocky 10:

```
ld.lld: error while loading shared libraries: libicui18n.so.70
```

`libxml2` is linked statically into `lld` for `lld-link`'s Windows manifests and drags ICU in
with it. The release is built on an Ubuntu carrying ICU 70; this box has ICU 74, and an ICU
soname cannot be bridged — the symbols are suffixed per major (`ucnv_open_70`), so ICU 74 exports
none of what `lld` wants, and a symlink would fail at load rather than work by luck. Every Linux
configure line passes `-fuse-ld=lld`, and clang resolves `ld.lld` from its own `bin` before
`PATH`, so this surfaces as `clang++: error: unable to execute command` at the **first link**,
not at configure. It reads like a missing linker and is a missing library.

**The first fix was to point `ld.lld` at the system LLD 21, and it silently cost LTO.** ELF links
all worked; bitcode did not — `ld.lld: error: Invalid summary version 14` — so
`check_ipo_supported()` flipped to `NO` and every configure printed `LTO requested but not
supported`. No Debug sanitizer job would notice, LTO being Release/Dist only, which is what made
it worth removing rather than documenting. BFD is no escape: the tarball ships `libLTO.so` but no
`LLVMgold.so`, so `ld.bfd` cannot load an LLVM plugin at all.

**So the fix is to supply ICU 70 rather than work around its absence.** `setup-olo-ci-host.sh`
builds ICU4C 70.1 from its pinned source release and drops three shared objects —
`libicuuc`, `libicui18n`, `libicudata` — into `/opt/llvm-23.1.0/lib`. Nothing else is needed:
every binary in the tarball already carries `RUNPATH '$ORIGIN/../lib'`, so there is **no wrapper,
no `patchelf`, no `LD_LIBRARY_PATH` and nothing on the system linker path**. Rocky's own ICU 74
is untouched and no other program on the host can see these. `ld.lld` is then the tarball's own
symlink to `lld` again, and both arms link with LLD 23.

Measured on the box: plain ELF, `-flto`, `-flto=thin`, `-fsanitize=address`, `-fsanitize=thread`
and `-fsanitize=undefined` all link and run through LLD 23. The ICU build takes a few minutes at
`-j4` (deliberately not `nproc` — this box runs CI) and links only libc, libstdc++ and libgcc.

**It degrades rather than fails.** A prefix whose `lld` will not start keeps the system LLD 21,
which links everything except LTO, and the script says so loudly. That is why the action's ladder
checks `ld.lld --version`: a pinned clang beside a linker that cannot run is a hard failure at
link time, and turning that into a warning is this action's whole job.

## The skew was never the cause of #1010's failures -- that argument still stands

Closing the skew removes a confound. It does not fix a known bug, and nobody should read this
change as having done so.

#1010 measured these jobs on the box, saw 215 failures against 1 hosted, and blamed half of them
on "clang 21 against gcc-toolset-15's libstdc++" -- two UBSan reports inside the standard library
(`downcast of misaligned address` in `hashtable_policy.h`, `unreachable program point` in
`stl_vector.h`). Two independent checks said otherwise, and still do:

- A ~200-line probe of `std::unordered_map` and `std::vector` under the exact
  `cmake/Sanitizers.cmake` UBSan flags is clean at `-O0` through `-O3`, with `_GLIBCXX_ASSERTIONS`,
  under **both** libstdc++ pairings the box offers. There was nothing wrong with the pairing.
- Both reports share one stack: `~ShaderLibrary` -> `~Ref<Shader>` -> `~OpenGLShader` ->
  `ShaderRegistry::UnregisterShader` walking a freed `unordered_map` — a destruction-order
  use-after-free in the engine, fixed on #1015's branch by making the registry never-destructed
  (the ADR 0004 shape). Any compiler reproduces it given a GL context; the hosted runner has none.

The other 212 were one bug too: SSBO binding slots 80..83 exceed Mesa's
`GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS` of 80 (NVIDIA reports 96), so `DDGI_ProbeMaintain` fails
to compile in `Renderer3D::Init` and asserts in a Debug build. Also fixed on that branch.

Two compiler versions in the fleet were real coverage, and that coverage is what this change
spends. It buys something narrower and more useful: when one arm goes red and the other does not,
the toolchain is no longer on the list of explanations.

## Facts about the box that decided this

- **The pinned clang selects a different libstdc++ from the system one.** Rocky's `clang++ 21` is
  pinned to gcc-toolset-15 by `/etc/clang/x86_64-redhat-linux-gnu-clang++.cfg`; the tarball's
  `clang++ 23` reads no such config and picks base GCC 14. Both link the same runtime
  `libstdc++.so.6.0.33`. One consequence is a simplification: base GCC 14 **has**
  `libstdc++exp.a`, which gcc-toolset-15 omits, so `check_linker_flag(CXX "-lstdc++exp")` in
  `OloEngine/CMakeLists.txt` now succeeds directly instead of falling through to the glob that
  hunts the base GCC lib dirs. That glob still earns its place -- the system clang is still the
  warn-down path.
- `std::forward_like` (the reason the hosted arm needs a non-default libstdc++) compiles against
  libstdc++ 14 and 15 alike, so it was never the box's problem. `libc++` is not installed
  system-wide and nothing here uses it.
- The tarball is ~2 GB compressed and ~12 GB unpacked; `/` on this box is 70 GB with ~59 GB free
  before the install.
- LLVM publishes no `SHA256SUMS`. It does publish a sigstore bundle
  (`LLVM-23.1.0-Linux-X64.tar.xz.jsonl`) whose SLSA in-toto statement carries the artifact
  digest; that attested value is what `setup-olo-ci-host.sh` pins, and it matches a `sha256sum`
  of the download.
- **The first self-hosted run after a compiler change is cold.** ccache keys on the compiler, so
  changing it invalidates the whole persistent cache; expect one slow round. The vcpkg binary
  cache is *not* affected -- the Linux jobs use the stock `x64-linux` triplet, whose ports are
  built with whatever compiler vcpkg detects rather than with `CMAKE_CXX_COMPILER`.

## Bumping the version

One number in three places, in this order: `llvm_version` in `scripts/setup-olo-ci-host.sh` plus
its `llvm_sha256` (take the digest from the release's `.jsonl` sigstore bundle, then confirm it
against a `sha256sum` of the download); `llvm_prefix` and `llvm_major` in
[`setup-linux-build`](../../.github/actions/setup-linux-build/action.yml); and the `version`
default in [`setup-llvm-apt`](../../.github/actions/setup-llvm-apt/action.yml) for the hosted
arm. Run the script on the box **before** merging the action change: the old prefix keeps working
until then, and the warn-and-fall-back path means a mismatch degrades rather than breaks.

A bump changes the prefix path, so the previous one is orphaned rather than replaced — 12 GB
sitting there. The script names it on every run instead of deleting it, because an unmerged
branch may still pin the old path and the script cannot know. `rm -rf /opt/llvm-<old>` once
nothing does.
