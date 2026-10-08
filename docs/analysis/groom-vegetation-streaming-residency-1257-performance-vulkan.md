# Vulkan monitored performance and collector limits (#1257)

Offline native counts, quantiles, logs and input/binary hashes are verified. [GL/region record](groom-vegetation-streaming-residency-1257-performance.md) preserves shared provenance, history and qualification.

Vulkan completes 16 retained/eight warm Deferred runs, all exit zero, on baseline
5dff103f and B e10f8fe1 (Release binary 8999466a...9317272). Two alternating AB/BA
pairs per workload retain 1996-2000 frame samples per camera. GPU validity/duplicate
exclusions are separate. Every capture audit reports foreign GPU activity;
qualifiedAt1Hz is false throughout. This is user-authorized loaded-host descriptive
evidence, not idle-host acceptance or a confidence/significance/causal verdict.

Per-run empirical p50/p95/p99 ranges in ms across the two runs:

| Vulkan workload / camera | A p50 / p95 / p99 ranges | B p50 / p95 / p99 ranges |
|---|---|---|
| groom / steady | 6.1780-6.4486 / 13.6857-14.2518 / 13.9021-14.5207 | 2.8000-5.6747 / 12.5645-14.3838 / 13.3656-14.5850 |
| groom / rapid-turn | 11.1551-12.2896 / 12.3312-15.2583 / 13.4643-17.4259 | 11.2065-12.2855 / 12.4600-15.3947 / 14.2880-17.9283 |
| groom / traversal | 11.3007-12.2388 / 12.7575-15.0331 / 14.2807-18.2116 | 11.2735-12.1075 / 12.4212-14.3221 / 13.8258-16.9121 |
| groom-pressure / steady | 3.6444-5.8843 / 5.2746-14.2713 / 6.1830-14.5893 | 5.9756-7.0480 / 14.2295-14.3228 / 14.5421-14.5818 |
| groom-pressure / rapid-turn | 11.1677-11.8183 / 12.3090-15.7611 / 14.3732-17.9966 | 11.1905-11.2196 / 12.4632-12.5877 / 13.9283-15.0202 |
| groom-pressure / traversal | 11.3013-11.4219 / 12.5186-13.1341 / 14.8608-15.4688 | 11.3071-11.4523 / 12.9545-12.9733 / 14.8191-14.8749 |
| vegetation / steady | 22.0443-22.1475 / 22.9885-23.3468 / 23.2078-27.5994 | 22.1521-22.3946 / 23.1426-23.5096 / 23.3886-23.9373 |
| vegetation / rapid-turn | 21.9973-22.0674 / 22.9670-23.1619 / 23.2116-23.4652 | 22.2134-22.2625 / 23.2330-23.3410 / 23.4392-23.6250 |
| vegetation / traversal | 21.7068-21.9847 / 22.8842-23.6419 / 23.2211-53.1547 | 22.1025-22.1855 / 23.3420-23.4412 / 23.6678-23.7490 |
| vegetation-pressure / steady | 22.0726-22.3623 / 23.0030-23.4082 / 23.2420-23.6272 | 4.7922-5.1471 / 14.4550-14.5506 / 14.7011-14.7733 |
| vegetation-pressure / rapid-turn | 22.0975-22.1091 / 23.0703-23.1173 / 23.2728-23.4721 | 11.2372-11.2593 / 12.3165-12.5775 / 13.0187-14.9242 |
| vegetation-pressure / traversal | 21.9671-21.9682 / 23.2083-23.2274 / 23.5109-23.6581 | 11.3444-11.3663 / 12.3227-12.4953 / 12.9759-13.1487 |

Normal vegetation p95 differences B-minus-A are positive both pairs for steady
(+0.1628/+0.1541 ms) and rapid-turn (+0.1791/+0.2660 ms), mixed for traversal.
Steady/traversal p99 changes are mixed, including high A tails; rapid-turn increases
+0.1598/+0.2276 ms. Numeric tails remain visible; no causal attribution follows.
Pressure renders lower detail, with B optional bytes zero and vegetation fallback
count four rather than one; it does not compare equal visual quality with eager A.
Groom pressure retains 41 fallback draws and two held completed CPU payloads.

| Vulkan workload | A tracker GPU / CPU stationary p50 bytes | B tracker GPU / CPU stationary p50 bytes |
|---|---|---|
| groom | 1234787728 / 0 | 1212750480 / 0 |
| groom-pressure | 1234787728 / 0 | 1211480976 / 0 |
| vegetation | 1407735136 / 0 | 1389612752 / 23609640 |
| vegetation-pressure | 1407735136 / 0 | 1314170688 / 25647994 |

Tracker backing is live plus retiring, views excluded; Vulkan uses VMA committed
backing. Whole-device NVML capture used bytes range 3,649,232,896-5,857,087,488 and
include desktop, other applications and driver reservations. They do not attribute
memory to this engine. Capture maxima are GPU 98%, read/write 46%, decoder 15%,
encoder 0%; memory utilization is not capacity. Changes are not a device-VRAM oracle.

## Collector cost and diagnostic limits

Both completed raw scans are bounded to one decoded record plus scalar aggregates
and four small collector-cost arrays. GL has 3287 samples, Vulkan 2998, contiguous
sequences, one start/end each, zero parse/timestamp errors. All device count/handle,
memoryInfo/utilization/decoder/encoder return codes are zero throughout.
Whole-collector costs below include warmup, setup and gaps, not measured frame costs:

| Backend / metric | p50 / p95 / maximum collector ms |
|---|---|
| GL / identityQueryElapsedMs | 145.0503 / 258.3428 / 1040.2772 |
| GL / nvmlQueryElapsedMs | 0.5215 / 0.7921 / 55.5093 |
| GL / sampleProcessingElapsedMs | 168.4155 / 290.2072 / 1074.9666 |
| GL / previousRecordSerializeWriteElapsedMs | 13.9512 / 26.2505 / 54.5646 |
| Vulkan / identityQueryElapsedMs | 136.2926 / 262.8322 / 1054.8435 |
| Vulkan / nvmlQueryElapsedMs | 0.2246 / 0.4063 / 48.9987 |
| Vulkan / sampleProcessingElapsedMs | 156.7305 / 289.7592 / 1094.7970 |
| Vulkan / previousRecordSerializeWriteElapsedMs | 12.4753 / 24.4511 / 58.9744 |

Nominal 1 Hz processing consumes measurable CPU time; this is not a zero-overhead
observer. Actual maximum gaps are 2.134583 s GL / 2.170687 s Vk. Sampling can miss activity
between timestamps. Processing excludes current serialization; the serialize/write
metric refers to the preceding record. Costs overlap nested queries and must not be
summed as independent work or subtracted from engine frame times.

Identity-query errors occur in 3225 GL / 2898 Vk records. Positive ownership categories
are only foreign/unknown_identity; no owned-editor positives or compositor attribution
is established. Unattributed activity is not assumed to be idle, DWM or engine work.
NVML-to-PDH adapter/LUID mapping is not established. Foreign names retain exact native
identity where available; different engine percentages are not additive.

All retained Release shader snapshots report unavailableInThisBuild, available false,
count null. Native logs/validation checks are separate; these results cannot establish
clean shader tracking. Earlier Debug GL zero counts and Vulkan unavailable tracking
keep their own provenance. The paired workloads cover Deferred; all-path functional,
region transitions, art/temporal and RT limits are in the other evidence records.
Read wall time includes OS caching; preparation includes CPU work. Baseline tier
telemetry is unavailable. Lower pressure tails are not seamless art acceptance.
