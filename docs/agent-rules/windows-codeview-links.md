# Measure Windows links before changing their resource policy

Keep `/Z7` and the build lock, measure the actual linker, and price GHASH against
the same input graph before enabling it by default (#1386).

`cmake/WindowsLinkResources.cmake` bounds clang-cl's `lld-link` worker pool with
`OLO_LINK_THREADS`. `auto` divides half the logical CPUs among the machine-wide
link permits, with a minimum of one worker. An explicit positive integer overrides
that choice; `0` retains lld's default for control measurements. This controls
worker threads, not a hard RSS limit or total process thread count. The existing
Ninja link pool and machine-wide semaphore still bound concurrent link processes.

`OLO_CODEVIEW_GHASH=ON` adds `-gcodeview-ghash` to Debug/Release compiles and
`/DEBUG:GHASH` to their links. It stays **OFF by default**. Changing it changes
compiler cache keys and makes the affected Windows objects cold once. The Windows
`build-memory.yml` cells compare OFF/ON without cache hits and publish compiler
RSS plus direct-link measurements. Those hosted cells omit USD and FFmpeg; they
are a reduced census, not proof that the full local USD link fits a 64 GiB host.
Default promotion needs full-input local measurements as well.

MSVC `link.exe` supports neither GHASH nor `/threads`; explicit requests for those
options are rejected. Its incremental linking remains enabled. The standalone
`/INCREMENTAL` flag is removed only for lld-link. Besides being unsupported by lld,
it selects CMake's incremental manifest-linking route, which can invoke the
linker twice. Preserve `/INCREMENTAL:NO` if explicitly supplied. Linux/DWARF
policy is separate and unchanged by this module.

## Collect a comparison

Set a fresh metrics directory for each cell and invoke the normal resource gate:

```powershell
$env:OLO_LINK_METRICS_DIR = Join-Path $PWD 'build-cached/link-metrics/control'
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6'
```

The link semaphore invokes `scripts/measure-link.ps1` while holding its permit.
Each JSON file records the command, response-file contents and hashes, direct
linker exit code, wall time, observed OS peak working set, and sampled maximum
thread count. A final unsampled memory/thread peak can be missed; neither figure
measures the whole process tree. Check successful exit, nonzero sampling, compiler
version, build configuration and input graph before comparing cells.
Metrics I/O is best-effort and preserves the linker's exit code. Missing output
or a response-file capture error invalidates the measurement; the Windows census
rejects those records rather than treating the successful build as timing evidence.

Use repeated one-build and two-build cells, including an idle baseline. A timing
collected next to another build is contended evidence. Count all linker invocations
for a target, including CMake's manifest relink. Retain binary/PDB hashes and verify
debugger symbol resolution after changing CodeView generation.

For an isolated measurement, pass `-MaxConcurrent 1` to the build gate and still
check host activity before and after the run. The gate checks every known slot,
serializes admission, and reserves both historical slots for an exclusive job.
This also prevents an older default two-slot wrapper from joining that job.
Older wrappers do not participate in the admission mutex; update them before
relying on the new admission protocol across every caller. The gate cannot exclude
unrelated workloads or builds that bypass it.

For cross-worktree cache checks, use a separate `CCACHE_STATSLOG` for each build;
global cache statistics include other sessions. Match compile inputs and investigate
misses before calling a second tree warm. Windows ASan uses **Release**, not the
unsupported Debug CRT; follow the [ASan recipe](build-trees-and-windows-asan.md).

## Worker count versus process thread count

`/threads:N` sets LLVM's worker strategy and the default ThinLTO job count. It is
not a hard cap on every native thread: LLVM 23.1.0 also opens input files through
Windows `std::async`, independently of that worker strategy. Retain the sampled
process thread maximum as a separate measurement; do not report it as bounded by
N. See the pinned [LLD driver implementation](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.0/lld/COFF/Driver.cpp#L143)
and its [thread-option handling](https://github.com/llvm/llvm-project/blob/llvmorg-23.1.0/lld/COFF/Driver.cpp#L1546).

## Local worker-limit measurement (2026-09-28)

Treat the worker limit as CPU admission, not a memory fix. On an i7-14700KF
(28 logical CPUs, 64 GiB), LLVM 23.1.0 replayed the full Debug test link from
`2662d9f1a`, including USD and FFmpeg, with GHASH off. The same response-file
inputs were used for three alternating repeats per cell; concurrent cells ran
two separate output links in each repeat. The exclusive build gate enclosed the
campaign and the normal link semaphore admitted both links. A five-second
known-process monitor found no competing build/test/editor in 54 samples.

| Worker option | Concurrent links | Links | Median seconds per link | Median peak MiB per link | Maximum sampled native threads |
|---|---:|---:|---:|---:|---:|
| default | 1 | 3 | 11.00 | 13,925 | 125 |
| `/threads:7` | 1 | 3 | 12.03 | 13,827 | 16 |
| default | 2 | 6 | 12.88 | 13,918 | 37 |
| `/threads:7` | 2 | 6 | 13.72 | 13,825 | 17 |

The first default link took 29.94 seconds and reached 125 sampled threads;
subsequent single-link default samples took 10.76 and 11.00 seconds. The median
does not hide that cold-start variation. Capping workers reduced median peak
working set by less than 1% and increased median link time by about 9% alone and
7% when paired. These are direct-link measurements, not whole-build throughput,
aggregate host memory, an ASan result, or a GHASH comparison. Raw commands,
response files, hashes, measurement records and host probes for PR #1520 are
retained locally under `build-cached/link-replays/`.

The Windows census must install the SDK's optional `com.lunarg.vulkan.debug`
component for ordinary Debug builds. Falling back to release SPIRV-Cross archives
fails with `_ITERATOR_DEBUG_LEVEL` 2 versus 0; a failed 2.39 GiB link is not a
successful memory measurement. The setup action's `windows-debug-libraries`
input installs and verifies the matching libraries. ASan uses the release CRT
and does not request that component.
