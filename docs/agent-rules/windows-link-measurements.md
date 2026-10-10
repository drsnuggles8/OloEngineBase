# Windows link measurements (appendix)

Dated tables behind the rules in [windows-codeview-links.md](windows-codeview-links.md). All on one
host: i7-14700KF (28 logical CPUs), 64 GiB, LLVM 23.1.0 `lld-link`, `/threads:7` (the `auto`
value) unless stated. "Peak" is the direct linker's OS peak working set from
`scripts/measure-link.ps1`; it is not host memory and not the process tree.

## GHASH, archive repetition and the split-binary floor (2026-10-09, #1386)

Inputs: `OloEngine-Tests` at `b7f0b07e0`, full input (USD, FFmpeg, OpenVDB, Alembic, MaterialX),
built in four `dev-cached` trees (`build-cached`, `build-cached-ghash`, `build-cached-asan`,
`build-cached-asan-ghash`). Each cell replays the response file `measure-link.ps1` recorded during
that tree's build, with outputs redirected. The eight worktree-shared archives (`bin/` and
`vendor/*-build`, shared until `31a8d20ea`) were copied per cell right after its build and
checked: `.debug$H` present in every ON engine archive, absent from every OFF one. Cells
alternate, inside `build-lock.ps1 -MaxConcurrent 1`. Another worktree's GL test runs were live in
every monitor sample of window A, so wall times are contended; the alternation spreads that over
both arms, and peak working set barely moves with CPU load.

| Config | Cell | Links | Median s | Min-max s | Median peak MiB |
|---|---|---:|---:|---:|---:|
| Debug | GHASH OFF | 3 | 17.57 | 15.30-42.49 | 14,331 |
| Debug | GHASH ON | 3 | 15.42 | 14.26-36.79 | 14,272 |
| Debug | OFF, each engine archive listed once | 3 | 14.76 | 14.74-16.14 | 14,327 |
| Debug | OFF, 12 test objects (main + helpers + one suite) | 3 | 10.01 | 9.66-10.76 | 9,136 |
| Release (ThinLTO) | GHASH OFF | 2 | 225.34 | 214.44-236.25 | 20,731 |
| Release (ThinLTO) | GHASH ON | 2 | 257.51 | 255.02-260.00 | 20,781 |
| Release + ASan (ThinLTO) | GHASH OFF | 2 | 669.49 | 612.35-726.63 | 27,913 |
| Release + ASan (ThinLTO) | GHASH ON | 2 | 614.02 | 611.33-616.71 | 28,055 |
| Release + ASan (ThinLTO) | OFF, `/opt:lldltojobs=2` | 1 | 624.85 | | 25,491 |

The `dev-cached-core` tree (`OLO_WITH_USD=OFF`, branch state after `31a8d20ea`) has no replay:
its in-build Debug test link peaked at 10,207 MiB in 36.9 s (contended), against the 14,331 MiB
full-input replay, -29%. In-build and replayed peaks agree within 0.2% (below).

The first link of each Debug arm was the cold one (42.5 s / 36.8 s); the rest took 14-18 s. The
ASan window (B) ran with the same GL test runs and also beside a full local test sweep; its
second-repeat times agree within 0.2% across GHASH arms. The `lldltojobs=2` row is one sample.
Ninja's in-build link of the same inputs peaked within 0.2% of the replays (Debug 14,348 /
14,284 MiB, Release 20,742 / 20,799, ASan 28,018 / 28,083 for OFF / ON).

What the input is. The Debug link reads 11.10 GB of files: 3.46 GB of test objects,
`usd_m.lib` 2.37 GB (force-loaded with `/WHOLEARCHIVE`), the three engine archives 2.33 GB,
`OloEditorCore.lib` 0.92 GB, and vcpkg/SDK archives for the rest. GHASH precomputes hashes only
in objects this tree compiles; USD, vcpkg and the Vulkan SDK archives carry none, and in Release
the engine archives are ThinLTO bitcode, which has no CodeView until the linker generates code.

Cross-archive references at `b7f0b07e0` (Debug, `llvm-nm`, symbols defined in the target part
and not in the source part): core to renderer 178 symbols from 53 objects, core to content 86
from 63, renderer to content 42 from 17, renderer to core 224, content to core 644, content to
renderer 285. The "core" part is every directory that is not Renderer or Content, so Terrain,
Physics3D, Gameplay and Networking supply most of its outbound edges.

## Worker limits (2026-09-28, #1520)

Full Debug test link based on `2662d9f1a` with local instrumentation changes, USD and FFmpeg
included, GHASH off, three alternating repeats per cell; concurrent cells ran two output links
per repeat. A five-second known-process monitor found no competing build, test or editor in 54
samples.

| Worker option | Concurrent links | Links | Median s per link | Median peak MiB per link | Max sampled threads |
|---|---:|---:|---:|---:|---:|
| default | 1 | 3 | 11.00 | 13,925 | 125 |
| `/threads:7` | 1 | 3 | 12.03 | 13,827 | 16 |
| default | 2 | 6 | 12.88 | 13,918 | 37 |
| `/threads:7` | 2 | 6 | 13.72 | 13,825 | 17 |

The first default link took 29.94 s and reached 125 sampled threads; later single default links
took 10.76 and 11.00 s. Capping workers moved median peak by under 1% and median time by +9%
alone, +7% paired.
