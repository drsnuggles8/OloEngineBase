# Windows links: memory follows link input, so cut the input

Keep `/Z7` and the build lock. A Windows test link's peak working set tracks what it is handed,
so cut the input (USD, the ThinLTO job count) rather than change how CodeView is merged. Every
number below is in [windows-link-measurements.md](windows-link-measurements.md) (#1386).

## The rules

- **`OLO_CODEVIEW_GHASH` stays OFF.** On the full-input test link it moved peak working set by
  -0.4% in Debug, +0.2% in Release and +0.5% in Release + ASan, and made the Release link 14%
  slower. Turning it on would make every Windows object cold once for that.
- **The largest local links are ThinLTO links.** `dev-cached` Release and Release + ASan compile
  the engine with `-flto=thin`, so the test link peaks at about 20.2 GiB (Release) and 27.3 GiB
  (ASan) against 14.0 GiB for Debug. Two ASan links at once leave a 64 GiB host about 9 GiB.
  The ThinLTO backend count follows `/threads:` (7 here); `/opt:lldltojobs=2` cut one ASan link
  to 24.9 GiB (-8.7%) at the same wall time. That is one sample, so it is a lever to measure
  before two ThinLTO links must share the host, not a default.
- **Build the core tests with `dev-cached-core`** when the change does not touch USD import.
  It sets `OLO_WITH_USD=OFF`; USD is force-linked into every executable with `/WHOLEARCHIVE`, no
  test depends on it, and CI already builds without it. Its Debug test link peaked at 10,207 MiB
  against 14,331 MiB for the full input (-29%) and discovered the same 9,900 test cases; its
  first build hit the compiler cache for 90% of TUs (ccache preprocessed mode, which ignores
  `-D`/`-I` a TU never expands). `OLO_WITH_USD` and USD's include directory now reach only the
  three importer TUs, which also stops USD's bundled oneTBB headers shadowing vcpkg's elsewhere.
- **Do not split `OloEngine-Tests` to save link memory.** A binary with 12 test objects still
  peaks at 8.9 GiB, 64% of the full link: the engine archives reference each other in a cycle
  (Scene and the asset layer call the renderer and the gameplay systems, and those call back), so
  any test that reaches `Scene` pulls in nearly everything. N binaries would pay that floor N
  times. Breaking the cycle is a layering redesign (#1574).
- **The repeated engine archives on the link line cost nothing on `lld-link`.** Listing each
  once left peak working set unchanged. They come from `LINK_INTERFACE_MULTIPLICITY 3`, which
  GNU `ld` (the `linux-gcc` presets) needs for the three-part cycle; leave it.
- **`OLO_LINK_THREADS` is CPU admission, not a memory cap.** `auto` gives each linker half the
  logical CPUs divided among the machine-wide link permits; `0` keeps lld's default for control
  measurements. Capping workers moved peak by under 1%.

MSVC `link.exe` supports neither GHASH nor `/threads`; explicit requests for those options are
rejected at configure time. Its incremental linking remains enabled. The standalone
`/INCREMENTAL` flag is removed only for lld-link: lld ignores it, and it selects CMake's
incremental manifest route, which can invoke the linker twice. `/INCREMENTAL:NO` is preserved
if supplied. Linux/DWARF policy is separate.

## Collect a comparison

Set a fresh metrics directory for each cell and build through the normal gate:

```powershell
$env:OLO_LINK_METRICS_DIR = Join-Path $PWD 'build-cached/link-metrics/control'
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6'
```

The link semaphore runs `scripts/measure-link.ps1` while holding its permit. Each JSON record
holds the command, response-file contents and hashes, exit code, wall time, observed OS peak
working set and sampled maximum thread count. A final unsampled peak can be missed, and neither
figure covers the process tree. A record with a nonzero exit, zero samples or a response-file
capture error is not a measurement.

To compare two configurations, build each in its own `build-cached-*` tree (the lock grants a
concurrent slot only to a command naming `build-cached`), then replay each recorded command from
its tree with `/out:`, `/pdb:` and `/implib:` redirected, alternating cells. Before `31a8d20ea`
the engine archives and vendored libraries were shared by every tree in a worktree; to replay
an older build, copy those inputs per cell first and check them (for GHASH, `.debug$H` present
or absent). Ninja's own in-build link of the same input peaks within 0.2% of a replay, so an
in-build record is a usable memory figure when a replay window is not available.

Run replays under `build-lock.ps1 -MaxConcurrent 1` and record host activity throughout; the
gate excludes builds, not another worktree's test or editor run. Before `49a9db178` an exclusive
request could starve: cached requests queued behind it took each slot as it freed. Wrappers in
other worktrees run their own copy of the script, so on a busy box expect to wait until theirs
include the fix. Announce a window on the issue before and after; it stalls every worktree's
builds.

For cross-worktree cache checks, use a separate `CCACHE_STATSLOG` per build. Windows ASan uses
**Release**, not the unsupported Debug CRT; follow the [ASan recipe](build-trees-and-windows-asan.md).
The Windows census needs the SDK's optional `com.lunarg.vulkan.debug` component for Debug
(release SPIRV-Cross archives fail on `_ITERATOR_DEBUG_LEVEL`); `setup-vulkan`'s
`windows-debug-libraries` input installs it.

## Worker count versus process thread count

`/threads:N` sets LLVM's worker strategy and the default ThinLTO job count. It is not a cap on
every native thread: LLVM 23.1.0 also opens input files through Windows `std::async`. Report the
sampled thread maximum separately, never as bounded by N. See the pinned
[LLD driver](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.0/lld/COFF/Driver.cpp#L143)
and its [thread-option handling](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.0/lld/COFF/Driver.cpp#L1546).
