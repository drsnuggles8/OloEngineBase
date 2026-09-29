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
| Developer box, RTX 4090 | NVIDIA 617.14, Vulkan 1.4.351 | satisfied | **run** (Debug) | **run**: ray query enabled | this PR, `vulkan-capability-report.rtx4090.md` |
| Hosted `windows-2025`, no GPU | none | no device | skip (`NOT EXERCISED - none of the 19 device-gated tests ran`) | skip | Windows.yml run 36510914603, shard 1 |
| Self-hosted Linux, AMD Navi 10 (RX 5600 XT), Mesa 25.2.7 | RADV / radeonsi | **refused** (`No device satisfies the ADR 0010 capability contract`) | skip, 149 tests | skip | gpu-conformance-amd run 36507240680; Mesa version from `docs/ops/self-hosted-gpu-runner.md` |
| Hosted Windows, Mesa lavapipe 26.2.0 (software) | llvmpipe, API 1.4.354 | satisfied | **run nightly**, `EXERCISED - 122/122 device-gated tests ran` | skip: lavapipe was not tested for ray query | vulkan-software.yml run 36522457757 |
| Developer box, live editor `--rhi vulkan` | same RTX 4090 | satisfied | n/a | ray query enabled (log: `Ray tracing: ray query enabled + ray-tracing pipeline`) | the editor picked the same device the report names; frame rendered, "Backend: Vulkan", zero errors or VUIDs in `OloEngine.log` |
| Mesa lavapipe 24.3.4 (software) | llvmpipe, API 1.3.296 | **refused**: API 1.4, descriptor heap, untyped pointers, device address commands | skip | skip | `vulkan-capability-report.lavapipe-24.3.4.md` |
| Mesa lavapipe 26.1.8 (software) | llvmpipe, API 1.4.354 | **refused**: descriptor heap, untyped pointers, device address commands | skip | skip | `vulkan-capability-report.lavapipe-26.1.8.md` |
| NVIDIA other than the 4090, AMD on Windows (any generation), Intel, Linux RADV 26.1+ | | **out of fleet** | | | no such machine exists in this repo's CI or on the developer box, so no claim is made about them (below) |

**Out of fleet.** The supported configurations are the ones a row above measured; the ADR 0010
amendment records that the floor is intentional and that the project claims nothing for a machine
that has not produced a report. To add a row, run the command at the top on that machine and paste
its output next to the reports here. The ADR's per-vendor driver notes are release-note history, not
support claims.

Two rules for reading it. A green `Windows.yml` says nothing about Vulkan: the banner in the log is
the evidence. And lavapipe is a software implementation; it finds spec drift and stale assertions,
not driver divergence, so it never counts as a second vendor.

## Who owns "a designated job must not silently skip"

`--olo-require-vulkan` (#1300, [testing-architecture.md §10](testing-architecture.md)) is the only
mechanism. It fails the first refused gate and makes `main` exit 3 when no device-gated test
executed. #1358 adds nothing parallel.

One boundary to know when a ray-query runner exists (#1294, still open; `Windows.yml` only
*reports* those suites): the tally counts a test skipped **past** the gate, such as the L7 fixtures
skipping on `!IsRayQueryEnabled()`, as not executed. So a designated ray-query job must select only
the L7 filter, `ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*`, and pass
`--olo-require-vulkan`. Mixed with other Vulkan suites the flag would pass on the others. Control
run on the RTX 4090, both from the same binary:

| Run | Result |
|---|---|
| L7 filter + `--olo-require-vulkan` | 25 of 26 executed and passed; the one skip is the opt-in `VegetationDetailedVersusCardExperiment` |
| same, with `OLO_VULKAN_NO_RAY_TRACING=1` | every L7 test skipped, **exit 3** |

## What is still open

- A hardware ray-query CI runner and a scheduled developer-box run: #1294 items 2 and 3. Not
  authorised or built here.
- The AMD self-hosted runner's verdict will change if its Mesa reaches a RADV with
  `VK_EXT_descriptor_heap` (ADR 0010 says Mesa 26.1). Re-run the report there before assuming.
