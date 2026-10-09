# Regions and monitored performance observations (#1257)

[Main analysis](groom-vegetation-streaming-residency-1257.md) and [public ledger](evidence/groom-vegetation-streaming-residency-1257.json) retain exact provenance/raw hashes.

## Monitored paired campaigns

The user authorized continuation with reported background load. GL and Vulkan each completed
16 retained runs and eight warm captures, all exit zero. Offline native verification
checks counts, quantiles, logs, binaries and 28 authored inputs; hashes are in the ledger.
A is baseline `5dff103f`; B is `e10f8fe1` with the same Release Editor binary as
`c519034ed` (`8999466a...9317272`). Functional and regional captures keep their own
c519 provenance. Both arms record compiler, input and cache signatures.
The audit rehashes recorded binaries/inputs; no complete shader/asset tree rehash is claimed.

Each backend uses four Deferred workloads, two fresh-process alternating AB/BA pairs,
and stationary/rapid-turn/traversal cameras. GL frame samples range 1994-2002 per
camera (Vulkan 1996-2000); GPU duplicates/exclusions remain separate. Native first/last CPU frame ranges
are retained. This is a descriptive sample of a loaded host, not idle-host acceptance,
a confidence interval, significance test or causal regression verdict.

| Backend | Retained / warm, exit status | Capture qualification | Whole-device used bytes min / max | Max counter gap |
|---|---|---|---|---|
| GL retry | 16 / 8, all 0 | All 16 foreign_gpu_activity_observed; 0 qualifiedAt1Hz | 3754577920 / 5236424704 | 2.134583 s |
| Vulkan | 16 / 8, all 0 | All 16 foreign_gpu_activity_observed; 0 qualifiedAt1Hz | 3649232896 / 5857087488 | 2.170687 s |

Named foreign activity includes Firefox, ChatGPT, Code Insiders, Claude, Explorer,
WebView2 and Steam. Unattributed positive samples remain unknown; process names do
not turn them into verified ownership. GL NVML capture maxima are GPU 100%, memory
read/write 42%, decoder 17%, encoder 0%. Device-wide memory includes other processes,
desktop and driver reservations; it is separate from physical engine backing.
Collector cadence is nominally 1 Hz and can miss intervening activity. Identity/NVML
query, processing and serialize/write costs are recorded in raw JSONL; no zero-cost
probe claim is made. Both raw logs have now been scanned one record at a time. Collector cost/ownership
limits and [Vulkan rows](groom-vegetation-streaming-residency-1257-performance-vulkan.md) are separate.

The table gives ranges across the two native per-run p50/p95/p99 values, in ms.
Full CPU/GPU/wait channels, paired differences and all raw values remain in the report.

| GL workload / camera | A p50 / p95 / p99 ranges | B p50 / p95 / p99 ranges |
|---|---|---|
| groom / steady | 2.2612-2.3187 / 3.1632-3.3674 / 3.9453-4.1108 | 2.0955-2.1410 / 2.7133-3.1452 / 3.1995-3.7298 |
| groom / rapid-turn | 11.2569-12.0944 / 12.6103-13.7506 / 14.5566-15.7692 | 11.0383-11.1288 / 12.2366-12.6309 / 13.3361-16.0865 |
| groom / traversal | 11.2898-11.9913 / 12.4052-13.4328 / 13.4042-15.4459 | 11.0552-11.1508 / 12.2420-12.3369 / 13.9084-14.5215 |
| groom-pressure / steady | 2.2359-2.2605 / 3.1000-3.4637 / 3.8075-4.5005 | 2.1603-2.2598 / 3.1757-3.1918 / 3.6830-3.8838 |
| groom-pressure / rapid-turn | 11.2323-11.4897 / 12.4739-12.7426 / 13.8515-14.0984 | 11.2392-11.4071 / 12.5233-12.8465 / 14.6853-14.9361 |
| groom-pressure / traversal | 11.2850-11.5053 / 12.5717-12.6282 / 13.6432-13.9343 | 11.1844-11.4062 / 12.3397-12.6496 / 13.7470-14.4836 |
| vegetation / steady | 30.5472-30.6496 / 31.0487-31.1472 / 31.2922-31.3635 | 30.3775-30.5508 / 31.1673-31.4072 / 31.7845-31.9982 |
| vegetation / rapid-turn | 30.2600-30.3382 / 30.7905-30.8933 / 31.0019-31.1597 | 30.2843-30.4026 / 32.2546-32.4247 / 34.4833-35.2355 |
| vegetation / traversal | 29.8385-29.8594 / 30.9682-30.9913 / 31.2098-31.2667 | 30.0894-30.1250 / 32.1299-34.1556 / 34.0840-39.1705 |
| vegetation-pressure / steady | 30.4520-30.5273 / 30.9072-31.0392 / 31.1231-31.2749 | 4.6378-4.6572 / 5.4840-5.6484 / 6.0137-6.2701 |
| vegetation-pressure / rapid-turn | 30.2897-30.3138 / 30.8120-30.8407 / 31.0144-31.0620 | 11.6209-11.6759 / 13.0409-13.0847 / 15.0717-15.3542 |
| vegetation-pressure / traversal | 29.7572-29.7736 / 30.8171-30.9294 / 31.1501-31.1806 | 11.7338-11.8463 / 13.1883-13.2871 / 14.3963-14.9938 |

GL vegetation normal has higher B p95/p99 in both pairs: rapid-turn p99 differences
are +3.3236/+4.2336 ms; traversal +2.8173/+7.9607 ms; steady +0.6347/+0.4923 ms.
These tail changes are retained, not hidden by similar medians or pressure results.
They do not establish engine causation under the observed loaded/partly unknown host.
Vulkan tails retain mixed changes; see the linked Vulkan record.

| GL workload | A tracker GPU / CPU stationary p50 bytes | B tracker GPU / CPU stationary p50 bytes |
|---|---|---|
| groom | 1007338438 / 9928479 | 985301782 / 9928479 |
| groom-pressure | 1007338438 / 9928479 | 984032582 / 9928479 |
| vegetation | 1185064777 / 10092225 | 1168002672 / 33701865 |
| vegetation-pressure | 1185064777 / 10092225 | 1092586664 / 35693779 |

Physical tracker accounting includes live plus retiring backing, excludes views and
is not all device VRAM. This branch corrects backing identity; its lower counters alone
are not a causal device-VRAM savings claim. Baseline has no streamingRawFile, so its
tier I/O/accounting is unavailable. Vulkan backing rows are in the linked record.

Pressure is a lower-detail workload: both B families have optional GPU bytes zero;
groom fallback draws rise 18 -> 41 and vegetation 1 -> 4. Held completed CPU payloads
remain 9,333,472 / 60,796,496 bytes (two / three loads), not active worker loads.
Vegetation's much lower pressure tails compare cutout floors with A's eager detail;
they are not equal-quality speedups. Release shader tracking is unavailable/null.

## Historical attempts retained

The first `paired-gl` campaign completes 16 retained/eight warm runs on c519034ed.
Its full numeric report remains descriptive: post-run GPU utilization is 35% with
Editor stopped; Firefox VideoDecode 20.42-26.60%, 3D 6.10-7.17%, and DWM 3D
22.98-26.15%. Those observations do not establish activity during every measured frame.
The original selected C++ process probes missed browser/desktop GPU engines.

The interrupted `paired-gl-monitored` attempt retains two A warm runs, both exit zero,
with collector exit zero/graceful token shutdown. It has no retained/report.json and
supplies no completed paired acceptance. Its cause is not inferred from these files.
The completed `paired-gl-monitored-retry` remains distinct from both earlier attempts.

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
