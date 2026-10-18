# Which machines can run the Vulkan backend and its tests

**Run `OloEngine-Tests --olo-vulkan-capability-report` before saying a machine can or cannot run a
Vulkan configuration; never infer it from a vendor, a card generation or an API version.** The
report evaluates every device with `VulkanCapabilities::Evaluate`, the reader the device pick and the
tests share, runs the real `VulkanDevice::Init` headless, and prints which test populations can
execute. Issue #1358 wrote it after finding that the contract list was single-sourced in code but
described by hand in five places.

```powershell
build\OloEngine\tests\Debug\OloEngine-Tests.exe --olo-vulkan-capability-report            # to stdout
build\OloEngine\tests\Debug\OloEngine-Tests.exe --olo-vulkan-capability-report=report.md  # to a file
```

It runs no tests and exits 0. Point `VK_DRIVER_FILES` at one ICD manifest to report one driver alone.
A software device (lavapipe, SwiftShader, WARP) is printed labelled `SOFTWARE` and is counted
separately from hardware.

## One list, everywhere

`VulkanCapabilities.cpp` holds the only table of contract rows. It drives `Evaluate`,
`RequiredDeviceExtensions()`, the `vkCreateDevice` feature chain (`VulkanContractFeatureChain`), the
refuse-to-init message, the bring-up tests and the report. To change the contract, amend
[ADR 0010](../adr/0010-vulkan-rhi-heap-bindless-only.md) first, then add or remove a row there;
`VulkanBringUp.CapabilityReportIsInternallyConsistent` reads the table, so it needs no edit. Before
#1358 the bring-up test hand-copied the three feature structs and its consistency check silently
omitted `VK_KHR_device_address_commands`.

## What each machine can run (measured)

Every row is a run or a CI log line. A row that is not measured says so.

| Machine | Driver | ADR 0010 contract | Device-gated Vulkan suites | L7 ray-query suites | Evidence |
|---|---|---|---|---|---|
| Developer box, RTX 4090 | NVIDIA 617.14, Vulkan 1.4.351 | satisfied | **run** (Debug) | **run**: ray query enabled | this PR, [vulkan-capability-report.rtx4090.md](vulkan-capability-report.rtx4090.md) |
| Hosted `windows-2025`, no GPU | none | no device | skip (`NOT EXERCISED - none of the 19 device-gated tests ran`) | skip | Windows.yml run 36510914603, shard 1 |
| Self-hosted Linux, AMD Navi 10 (RX 5600 XT), RADV, Mesa 25.2.7 | radv 25.2.7, API 1.4.318 | **refused**: no `VK_EXT_descriptor_heap`, `VK_KHR_shader_untyped_pointers` or `VK_KHR_device_address_commands`; the box also exposes llvmpipe 25.2.7, refused for the same three | skip (149 device-gated tests skip in the nightly) | skip: RADV lists no ray-query extensions | gpu-conformance-amd run 36621553072, [vulkan-capability-report.amd-navi10-radv-25.2.7.md](vulkan-capability-report.amd-navi10-radv-25.2.7.md); nightly skips: run 36507240680 |
| Hosted Windows, Mesa lavapipe 26.2.0 (software) | llvmpipe, API 1.4.354 | satisfied | **run nightly**, `EXERCISED - 122/122 device-gated tests ran` | **run nightly, required** (#1294): the real gate enables ray query; 34 of 35 L7 cases pass, the one skip is the opt-in vegetation experiment, and a case that does not execute fails the job | vulkan-software.yml runs 36522457757 (nightly) and 36621566529 (L7 filter dispatch); [lavapipe-ray-query-suites.md](lavapipe-ray-query-suites.md) |
| Developer box, live editor `--rhi vulkan` | same RTX 4090 | satisfied | n/a | ray query enabled (log: `Ray tracing: ray query enabled + ray-tracing pipeline`) | the editor picked the same device the report names; frame rendered, "Backend: Vulkan", zero errors or VUIDs in `OloEngine.log` |
| Mesa lavapipe 24.3.4 (software) | llvmpipe, API 1.3.296 | **refused**: API 1.4, descriptor heap, untyped pointers, device address commands | skip | skip | [vulkan-capability-report.lavapipe-24.3.4.md](vulkan-capability-report.lavapipe-24.3.4.md) |
| Mesa lavapipe 26.1.8 (software) | llvmpipe, API 1.4.354 | **refused**: descriptor heap, untyped pointers, device address commands | skip | skip | [vulkan-capability-report.lavapipe-26.1.8.md](vulkan-capability-report.lavapipe-26.1.8.md) |
| NVIDIA other than the 4090, AMD on Windows (any generation), Intel, Linux RADV 26.1+ | | **out of fleet** | | | no such machine exists in this repo's CI or on the developer box, so no claim is made about them (below) |

**Out of fleet.** The supported configurations are the ones a row above measured; the ADR 0010
amendment records that the floor is intentional and that the project claims nothing for a machine
that has not produced a report. To add a row, run the command at the top on that machine and paste
its output next to the reports here. The ADR's per-vendor driver notes are release-note history, not
support claims.

Two rules for reading it. A green `Windows.yml` says nothing about Vulkan: the banner in the log is
the evidence. And lavapipe is a software implementation; it finds spec drift and stale assertions,
not driver divergence, so it never counts as a second vendor.

Raw reports: [RTX 4090](vulkan-capability-report.rtx4090.md), [lavapipe 26.2.0](vulkan-capability-report.lavapipe-26.2.0.md), [AMD Navi 10 RADV](vulkan-capability-report.amd-navi10-radv-25.2.7.md).

## Who owns "a designated job must not silently skip"

`--olo-require-vulkan` (#1300, [testing-architecture.md §10](testing-architecture.md)) is the only
mechanism. It fails the first refused gate and makes `main` exit 3 when no device-gated test
executed. #1358 adds nothing parallel.

One boundary: the tally counts a test skipped **past** the gate, such as the L7 fixtures skipping
on `!IsRayQueryEnabled()`, as not executed. Mixed with other Vulkan suites, `--olo-require-vulkan`
passes on the others. That is why `vulkan-software.yml` requires the L7 cases per case from the
gtest XML ([lavapipe-ray-query-suites.md](lavapipe-ray-query-suites.md)), not through the flag.
Control run on the RTX 4090, both from the same binary, with only the L7 filter selected:

| Run | Result |
|---|---|
| L7 filter + `--olo-require-vulkan` | 25 of 26 executed and passed; the one skip is the opt-in `VegetationDetailedVersusCardExperiment` |
| same, with `OLO_VULKAN_NO_RAY_TRACING=1` | every L7 test skipped, **exit 3** |

## Ray query on lavapipe, measured

The repo's earlier statement that the L7 suites "skip on lavapipe like everywhere else" was never
tested (#1320 says so). Measured on Mesa 26.2.0 with `vulkan-software.yml` dispatched with
`gtest_filter=ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*` and `--olo-require-vulkan`
(run 36621566529): the gate enables ray query, and the run reported `PARTIAL - 25 of 26 device-gated
tests ran` (the one skip is the opt-in `VegetationDetailedVersusCardExperiment`).

| Suite | Result on lavapipe 26.2.0 |
|---|---|
| `RayTracingDevice.*` | passes (acceleration-structure build, compaction, traces, hit normals) |
| `GpuPathTracerDevice.*` | passes, including the CPU-reference Cornell box agreement |
| `ReSTIRPTDevice.*` | 1 of 9 passes; the other 8 fail at `vkWaitForFences(..., 10 s)` returning `VK_TIMEOUT` after 54-77 s each |

The 8 failures were not slow compute. llvmpipe compiles the fragment shader inside the first
draw's fence wait, about 40 s, and every later draw takes under 6 ms. #1294 gave the fixture a
fence budget derived from the device and confirmed that the #1288 oracles still go red on lavapipe
under the stale flags lane. All 9 now pass there:
[lavapipe-ray-query-suites.md](lavapipe-ray-query-suites.md).

## What is still open

- No hardware ray-query CI runner and no scheduled developer-box run: the user declined both on
  2026-10-18 (#1294). lavapipe is the scheduled RT run.
- The AMD self-hosted runner's verdict will change if its Mesa reaches a RADV with
  `VK_EXT_descriptor_heap` (ADR 0010 says Mesa 26.1). Re-run the report there before assuming.
