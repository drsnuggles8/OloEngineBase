# Regions and qualified performance observations (#1257)

[Main analysis](groom-vegetation-streaming-residency-1257.md) and [public ledger](evidence/groom-vegetation-streaming-residency-1257.json) retain full provenance/raw hashes.

## Host-load qualification

Timing captures are paused. The first OpenGL AB/BA campaign completed 16 retained runs
plus eight warm captures, exit zero, but is **not verified idle-host acceptance**. The
post-run nvidia-smi sample shows 35% GPU utilization with Editor stopped. Three GPU
Engine samples identify Firefox PID 5264 VideoDecode 20.42-26.60%, 3D 6.10-7.17%, and
DWM PID 2260 3D 22.98-26.15%. These are post-run samples, not evidence that the same
activity persisted throughout every measured frame. Existing host probes check selected
C++/build/editor competitors and miss browser/desktop GPU engines. No causal speedup
or regression attribution is made. User direction/host qualification is pending. Paired
Vulkan has never run; full accepted performance remains pending.

## Completed region campaign

Source c519034ed and Release binary 8999466a...9317272 complete all four Deferred cells,
controller exit zero. Each completes 3000 steps (1000 per camera). Actual native editor
samples total 23,998: GL 5999/cell, Vulkan 6000/cell; every camera has contiguous CPU
frame IDs. Native logs have zero word-error/VUID matches. Before/after host probes have
no selected competitors, but no continuous GPU-load qualification; these tails carry
that limitation too.

| Backend / family | First pending / load / unload CPU frames | Loaded min / max / last | Pending min / max / last | Budget evictions |
|---|---|---|---|---:|
| opengl groom | 5676 / 5677 / 6252 | 1 / 2 / 1 | 0 / 1 / 0 | 0 |
| opengl vegetation | 5078 / 5079 / 5635 | 1 / 2 / 1 | 0 / 1 / 0 | 0 |
| vulkan groom | 5682 / 5683 / 6258 | 1 / 2 / 1 | 0 / 1 / 0 | 0 |
| vulkan vegetation | 5050 / 5051 / 5608 | 1 / 2 / 1 | 0 / 1 / 0 | 0 |

First-region Ready precedes measurement. Logs separately name Ready/unloaded regions,
without exported per-frame IDs. Proximity unload decreases loaded count but does not
increment count/byte-budget eviction counters; zero does not mean no unload. Original
vegetation null-read crashes and corrected red/green regression remain preserved.

| Backend / family / camera | Samples | Frame p50 / p95 / p99 ms |
|---|---:|---|
| opengl groom steady | 1999 | 3.3643 / 3.9953 / 4.3335 |
| opengl groom rapid-turn | 2000 | 12.0053 / 13.2776 / 16.3816 |
| opengl groom traversal | 2000 | 12.0522 / 13.0131 / 14.003 |
| opengl vegetation steady | 2000 | 32.1065 / 32.8947 / 33.3874 |
| opengl vegetation rapid-turn | 2000 | 30.8976 / 32.6825 / 34.3901 |
| opengl vegetation traversal | 1999 | 31.6404 / 34.8442 / 37.0571 |
| vulkan groom steady | 2000 | 6.1059 / 14.0413 / 14.2725 |
| vulkan groom rapid-turn | 2000 | 12.2652 / 13.9004 / 17.3124 |
| vulkan groom traversal | 2000 | 12.5885 / 13.992 / 15.9528 |
| vulkan vegetation steady | 2000 | 22.0571 / 22.6108 / 22.8774 |
| vulkan vegetation rapid-turn | 2000 | 21.8817 / 22.483 / 22.7193 |
| vulkan vegetation traversal | 2000 | 22.2389 / 24.0193 / 24.4625 |

| Backend / family | Physical GPU backing min / max bytes | Pending CPU max bytes | Sampled read bytes / us | Uploaded bytes / upload us |
|---|---|---:|---|---|
| opengl groom | 979833415 / 990029062 | 57024224 | 0 / 0 | 4607728 / 1812 |
| opengl vegetation | 1183970628 / 1400903136 | 56924080 | 11300371 / 6149 | 48607712 / 501493 |
| vulkan groom | 1203726736 / 1220735600 | 57024224 | 0 / 0 | 4607728 / 1033 |
| vulkan vegetation | 1405590880 / 1623217296 | 56924080 | 11300371 / 5694 | 48607712 / 464120 |

GPU backing is live plus retiring, views excluded: GL format estimates / Vulkan VMA
committed backing, not a full VRAM census. Cost deltas exclude pre-first-sample work and
include inter-camera gaps. Preparation includes CPU build/I/O; read wall time includes
OS caches, not isolated disk latency. Groom prepares from already resident sources.
Per-camera preparation, budgets, upload sums and owner reconciliation remain in JSON.

## First GL AB/BA data: descriptive only

A is baseline 5dff103f...7260ef; B is c519034ed. Exact binaries/compiler/input hashes
and dirty-diff signatures are retained. Four workloads have two fresh-process pairs
each in alternating AB/BA order. Below, each triplet gives the **range across the two
runs** of native frame p50 / p95 / p99, in ms. It is not a confidence interval or effect
estimate. Raw reports separately retain paired differences, GPU exclusions and CPU
versus GPU backing. Baseline has no streamingRawFile; its tier telemetry is unavailable.

| Workload / camera | A p50 / p95 / p99 ranges | B p50 / p95 / p99 ranges |
|---|---|---|
| groom / steady | 2.1722-2.1738 / 2.9495-3.0225 / 3.5376-3.8165 | 2.0890-2.1182 / 2.8251-2.8391 / 3.3506-3.4410 |
| groom / rapid-turn | 10.9601-11.1139 / 11.9618-12.3748 / 13.0478-13.4926 | 10.8910-10.9171 / 11.7870-12.0091 / 12.4483-13.1903 |
| groom / traversal | 10.9858-11.1015 / 11.9686-12.0350 / 12.7642-13.1197 | 10.9457-10.9690 / 11.8876-12.0679 / 13.0363-13.2928 |
| groom-pressure / steady | 2.1516-2.1529 / 2.9723-3.0556 / 3.7331-3.8203 | 2.0862-2.0916 / 2.8039-2.9346 / 3.3276-3.5432 |
| groom-pressure / rapid-turn | 10.9051-11.0407 / 11.8761-11.9948 / 12.3966-12.7837 | 10.7979-11.0588 / 11.8529-12.1548 / 13.0910-13.6622 |
| groom-pressure / traversal | 11.0760-11.2287 / 12.1050-12.2693 / 12.7308-13.3523 | 10.8838-11.0196 / 11.9565-12.0296 / 12.7877-13.1234 |
| vegetation / steady | 30.7698-30.9641 / 31.1619-31.3529 / 31.3711-31.5949 | 30.8391-31.0461 / 31.4642-31.7862 / 31.8002-32.2244 |
| vegetation / rapid-turn | 30.6394-30.7009 / 31.0801-31.1542 / 31.2804-31.4294 | 30.5486-30.8532 / 31.4768-31.8673 / 32.0212-32.4399 |
| vegetation / traversal | 30.1420-30.1470 / 31.2525-31.3098 / 31.5171-31.5580 | 30.4010-30.5879 / 31.7788-32.0963 / 32.3767-32.5980 |
| vegetation-pressure / steady | 30.6786-30.9742 / 31.0594-31.3687 / 31.3588-31.6391 | 4.6515-4.7168 / 5.5691-5.7068 / 5.9183-6.1547 |
| vegetation-pressure / rapid-turn | 30.4901-30.7146 / 30.9592-31.1631 / 31.1998-31.4072 | 10.9040-11.5453 / 11.8334-12.6401 / 12.5382-14.8250 |
| vegetation-pressure / traversal | 29.9953-30.1621 / 31.0890-31.3258 / 31.3162-31.6071 | 11.2772-11.9688 / 12.3853-13.7464 / 13.4433-15.5083 |


Reported stationary backing p50 ranges across the two runs (bytes):

| Workload | A GPU / tracked CPU | B GPU / tracked CPU |
|---|---|---|
| groom | 1007338438 / 9928479 | 985301782 / 9928479 |
| groom-pressure | 1007338438 / 9928479 | 984032582 / 9928479 |
| vegetation | 1185064777 / 10092225 | 1168002672 / 33701865 |
| vegetation-pressure | 1185064777 / 10092225 | 1092586664 / 35693779 |

These are reported tracker counters; this branch also corrects physical backing
identity. Differences alone are not a device-VRAM saving oracle. Per-camera backing,
peak values and separate CPU/GPU timing channels remain in the JSON/native report.

Each retained camera has at least 1000 frame samples; GPU validity/duplicate exclusions
apply separately. Before/after selected-process probes were clear; the post-run GPU
observations leave idle-host acceptance unverified. Warm captures exist, but do not
resolve background interference. Two pairs provide descriptive run-level observations
only; no significance, confidence or final regression verdict is claimed. Raw numeric
values are preserved even where tails are high. No paired Vulkan or accepted whole-
matrix performance result exists yet.
