# The L7 ray-query suites on lavapipe

**The three L7 ray-query device suites run nightly on Mesa lavapipe, and the job fails when any of
their cases does not execute.** `RayTracingDevice.*`, `GpuPathTracerDevice.*` and
`ReSTIRPTDevice.*` ride in `vulkan-software.yml`'s filter. The step after the suites,
*Require every L7 ray-query case to execute*, runs `scripts/vulkan_ray_query_coverage.py
--require` over the gtest XML. Issue #1294. The user decided on 2026-10-18 that lavapipe is the
only scheduled RT run: no self-hosted RTX runner, no scheduled task on a developer box.

## 1. Why `--olo-require-vulkan` is not enough

`--olo-require-vulkan` fails a test whose **device** the gate refused. A ray-query fixture skips
*after* that, inside `SetUp`, when `IsRayQueryEnabled()` is false. That is a clean `GTEST_SKIP`,
and in a run that also executes the `Vulkan*` suites the banner reads `PARTIAL` and `main` exits 0.
So the per-case requirement has to come from the XML.

The XML alone cannot see a case that a `--gtest_filter` exclusion dropped, because such a case is
absent rather than skipped. The gate therefore takes the defined case set from
`--gtest_list_tests` over the three suites and reports any listed case missing from the XML as
*did not run*.

**One exemption:** `RayTracingDevice.VegetationDetailedVersusCardExperiment` runs only with
`--olo-vegetation-experiment` (#1240). Add another `--allow-skip` only for a case that is opt-in by
design, and say why in the workflow comment.

`Windows.yml` runs the same script without `--require`, as a report: hosted Windows has no
ray-query device, so skipping there is expected.

## 2. A device test's fence budget comes from the device

**Use `DeviceFenceWaitBudgetNs(physicalDevice)` from `tests/Rendering/VulkanTestSupport.h` for a
device test's fence wait, not a literal 10 s.** It gives hardware 10 s and a software ICD 300 s,
classified by `VulkanCapabilities::Evaluate(...).IsSoftware`, the same rule the capability report
uses.

The reason: **llvmpipe compiles a fragment shader variant lazily, on the queue thread, at its first
draw.** That draw's fence wait pays the whole LLVM compile. Measured on lavapipe 26.2.0 with
`ReSTIR_PT.glsl` on a 16×16 target, with the wait temporarily unbounded:

| Submit (per fresh device) | Record | Fence wait |
|---|---:|---:|
| 0: table upload | 93-101 ms | 1.5 s |
| 1: first `ReSTIR_PT` draw | 1.9-2.4 s | **38-41 s** |
| 2-6: later draws, other stages | 0 ms | 0.3-5 ms |

The work itself is not slow. Before the budget, 8 of 9 `ReSTIRPTDevice` cases died at the
fixture's own `vkWaitForFences` (`ReSTIRPTDeviceTest.cpp`, `RecordAndSubmit`) on their first
draw, with `VK_TIMEOUT` after 40-60 s each. Neither `VulkanOneShot` nor the context flush was
involved. #1358 had read this as a wait timeout on slow software compute. **Time the first submit
apart from the later ones before shrinking a workload for a software driver.**

## 3. The oracles still discriminate on lavapipe

Nothing was shrunk, but a software device could still pass a case for the wrong reason. Negative
control, 2026-10-18: `kClosureV2GBufferFlagsLane` replaced by the stale `2.0` lane from #1288,
`ReSTIRPTDevice.*` on lavapipe:

| Arm | Reference | Observed | Verdict |
|---|---:|---:|---|
| near-specular metal furnace (R) | 0.650 | 0.0095 | red (1.45%, #1288's figure) |
| glossy closed Cornell, initial | 0.113 | 0.037 | red |
| glossy closed Cornell, temporal + spatial | 0.113 | 0.037 | red |
| the other 7 cases | | | green |

With the real lane all 9 pass on lavapipe and on an RTX 4090.

## 4. Reproduce locally

```powershell
# Mesa 26.2.0 from pal1000/mesa-dist-win, extracted anywhere.
$env:VK_DRIVER_FILES = "<mesa>\x64\lvp_icd.x86_64.json"; $env:VK_ICD_FILENAMES = $env:VK_DRIVER_FILES
cd OloEditor
..\build-cached\OloEngine\tests\Release\OloEngine-Tests.exe --olo-require-vulkan `
  --gtest_filter='RayTracingDevice.*:GpuPathTracerDevice.*:ReSTIRPTDevice.*' --gtest_output=xml:rt.xml
```

Set the variables from PowerShell. From Git Bash the same run printed `vkCreateInstance failed (no
Vulkan 1.4-capable ICD available)`. Release, 2026-10-18: 34 of 35 passed in 561 s on lavapipe and
69 s on the RTX 4090, and the one skip was the opt-in.

## 5. Negative controls for the gate

- **Ray query off:** set `OLO_VULKAN_NO_RAY_TRACING=1` locally, or dispatch the workflow with
  `ray_query_off: true`. Locally, with `Vulkan*` in the same run: the banner read
  `PARTIAL - 138 of 178 device-gated tests ran`, which `--olo-require-vulkan` accepts. The
  capability report said `Ray query enabled: **no** (disabled by OLO_VULKAN_NO_RAY_TRACING=1)`,
  and the gate exited 1 with `34 required case(s) did not execute`.
- **A case filtered out:** dispatch with a `gtest_filter` that excludes one required case. The
  suites step stays green, and the gate fails and names the case as *did not run*.
