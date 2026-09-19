# Hosting the Vulkan suite on a software driver (lavapipe)

**Mesa's lavapipe can host this engine's Vulkan backend, but only from Mesa 26.2.0 onward — and the
thing that decides it is the ADR 0010 extension contract, not the API version.** Check
`VK_EXT_descriptor_heap`, `VK_KHR_shader_untyped_pointers` and `VK_KHR_device_address_commands`
against the exact Mesa build before assuming a software Vulkan run is possible; a 1.4 `apiVersion`
means nothing on its own. `.github/workflows/vulkan-software.yml` is the job this rule produced
(issue #1301).

## The check, in one command

```powershell
$env:VK_DRIVER_FILES = '<mesa>\x64\lvp_icd.x86_64.json'   # VK_ICD_FILENAMES on an older loader
& "$env:VULKAN_SDK\Bin\vulkaninfoSDK.exe" | Select-String 'descriptor_heap|untyped_pointers|device_address_commands'
```

Three hits means the contract in `VulkanCapabilities::RequiredDeviceExtensions()` can be satisfied;
fewer means the gate will refuse and every device-gated Vulkan test will skip. `vulkaninfo` also
prints the feature bits (`descriptorHeap`, `shaderUntypedPointers`, `deviceAddressCommands`), which
the contract requires separately from the extension being listed — lavapipe reports all three
`true`, but a driver that lists an extension with its feature bit `false` is refused, and only the
feature dump shows that.

## What was measured, and on which Mesa

Probed with Vulkan SDK 1.4.357.0 on Windows, `mesa-dist-win` release-msvc archives, ICD selected
with `VK_DRIVER_FILES` so nothing else could answer:

| | Mesa 24.3.4 (the llvmpipe GL job's pin) | Mesa 26.1.8 | Mesa 26.2.0 |
|---|---|---|---|
| lavapipe `apiVersion` | 1.3.296 | **1.4.354** | **1.4.354** |
| `VK_KHR_swapchain` | yes | yes | yes |
| `VK_EXT_descriptor_heap` | no | **no** | **yes** (`descriptorHeap = true`) |
| `VK_KHR_shader_untyped_pointers` | no | **no** | **yes** (`shaderUntypedPointers = true`) |
| `VK_KHR_device_address_commands` | no | **no** | **yes** (`deviceAddressCommands = true`) |
| `VK_KHR_unified_image_layouts` (optional) | no | yes | yes |
| ADR 0010 contract satisfied | no (API version too) | **no** | **yes** |

So 26.2.0 is a hard floor, not a preference. The interesting row is 26.1.8: it clears the Vulkan
1.4 bar that #1301 opened by naming as the known blocker, and still fails the contract on all three
extensions. **The API version was the visible gate and not the binding one.** Anyone bumping the
pin backwards to match cross-vendor.yml's GL archive will get a job that skips everything.

## Why a skipped run must fail here, and what already does it

`OLO_VULKAN_DEVICE_OR_SKIP()` skips cleanly when no device satisfies the contract. In a developer's
run that is right. In a job whose entire purpose is the Vulkan coverage it is the failure mode that
matters most: the binary exits 0, gtest prints `[ PASSED ]`, and the job is green having verified
nothing — which is strictly worse than not having the job, because it converts "nobody is watching"
into "something is watching" without the second being true.

`--olo-require-vulkan` (issue #1300) is the mechanism, and the software job must pass it:

- the gate turns its first refusal into a `FAIL()`, naming the missing capability, and
- `main` returns **3** when `--olo-require-vulkan` was given and *no* device-gated test executed —
  the case the gate cannot see, such as a `--gtest_filter` that selected none of them.

Do not invent a second mechanism (an executed-test floor parsed out of the XML, a grep over the
banner). Extend `VulkanCoverage` instead; `FormatBanner` is a pure function over a `Tally` and is
tested device-free.

The A/B, run against Mesa 24.3.4's lavapipe — a real driver below the contract, not a broken path:

```text
WITH --olo-require-vulkan     exit 1   [  FAILED  ] 1   Vulkan backend coverage: NOT EXERCISED
WITHOUT it                    exit 0   [  SKIPPED ] 1   Vulkan backend coverage: NOT EXERCISED
```

Same driver, same filter, same banner. Only the exit code differs, and the exit code is the only
part of it CI reads.

## What it costs, and what it caught

Measured on a developer box, **Debug**, whole Vulkan filter (207 cases, 105 device-gated), NVIDIA
RTX 4090 as the control arm and lavapipe selected with `VK_DRIVER_FILES`:

| | NVIDIA RTX 4090 | lavapipe 26.2.0 |
|---|---|---|
| `VulkanPassSuite` alone (57 device-gated) | 100.6 s | **28.3 s** |
| whole Vulkan filter | 190 s | **118 s** |

**Software Vulkan is not the slow arm here.** These suites are 128×128 targets and one-group
dispatches, so the run is launch-bound rather than fill-bound and llvmpipe's per-call cost beats a
discrete GPU's submission latency. Do not budget this job against the ~25 min the llvmpipe *GL*
job takes — that one rasterises real scenes.

Two real differences surfaced the first time the suite met a software driver, and both are the
reason the job is worth its minutes:

1. **An engine defect, now fixed.** `VulkanRendererAPI::SetViewport` emitted the plain
   `vkCmdSetViewport`, while every pipeline the backend builds declares
   `VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT`. Setting a state that is *static* in the bound pipeline is
   inert on a driver at conformance version 1.3.8.0 or newer — so NVIDIA never said a word — and a
   validation **error** on one below it. Three `VulkanPassSuite` tenants failed their
   zero-validation-errors assertion on lavapipe and nowhere else.
2. **A lavapipe defect, excluded in the workflow.**
   `VulkanDrawPath.FramebufferBlitPreservesUnrequestedDepthStencilAspects` asserts that a
   single-aspect copy or resolve onto a combined depth/stencil image leaves the other aspect
   untouched. lavapipe overwrites it: all 8 depth-only permutations lose the destination's stencil
   and the stencil-only ones symmetrically lose its depth, on all 256 pixels, with **no validation
   message at all** — legal usage, wrong result. The same binary passes all 24 permutations on
   NVIDIA. Re-test on the next Mesa bump and drop the exclusion when it passes.

Note the asymmetry: (1) is the job doing its job, and (2) is the tax for using a software driver.
Both look identical in a red run, so attribute a new failure against the hardware arm before
believing it is an engine regression.

## Traps

- **The ICD manifest is the thing you point at, not the DLL.** `vulkan_lvp.dll` is loaded by the
  *relative* `library_path` inside `lvp_icd.x86_64.json`. Copying the DLL next to the test binary
  the way cross-vendor.yml copies `opengl32.dll` does nothing whatsoever — the GL driver is picked
  up by DLL search order, the Vulkan one is not.
- **Set both `VK_DRIVER_FILES` and `VK_ICD_FILENAMES`.** The first is current, the second is its
  deprecated predecessor, and which a given `vulkan-1.dll` honours depends on the loader version
  the SDK installed. Naming one ICD explicitly also makes the run deterministic on a box that has
  a hardware ICD registered — without it, lavapipe is one candidate among several and the local
  probe silently measures the GPU instead.
- **…and do not rely on them in CI: register the ICD in the registry too.** The loader reads both
  variables through its *secure-getenv* path, which returns nothing for a process running with
  elevated privileges — and GitHub's Windows runners run steps as an administrator. The loader then
  falls back to registry enumeration, finds no driver on a GPU-less runner, and `vkCreateInstance`
  returns `ERROR_INCOMPATIBLE_DRIVER`:

  ```text
  ERROR: [Loader Message] windows_read_data_files_in_registry: Registry lookup failed to get
         ICD manifest files.  Possibly missing Vulkan driver?
  ERROR: vkCreateInstance failed with ERROR_INCOMPATIBLE_DRIVER
  ```

  **This cannot be reproduced on a developer box**, where the shell is not elevated and the
  variables work exactly as documented — which is what makes it worth writing down rather than
  rediscovering. The fix is the loader's own Windows discovery mechanism:

  ```powershell
  New-Item -Path 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' -Force | Out-Null
  New-ItemProperty -Path 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers' -Name $icd -PropertyType DWord -Value 0 -Force
  ```

  Keep the env vars as well: they cost nothing and they are what makes a local repro behave.
- **A software Vulkan run still needs software OpenGL.** The Vulkan suites live in the shared test
  process, which brings up a GL context for the rest of the binary and restores it around every
  `ScopedVulkanRenderCommandSelection`. The same Mesa archive ships both, so this costs one extra
  `Copy-Item`, but leaving it out means fighting a missing GL context while trying to prove
  something about Vulkan.
- **RADV is not lavapipe.** The self-hosted AMD box cannot host this suite, and the reason is the
  same row of the table: RADV does not implement `VK_EXT_descriptor_heap`. A future RADV that does
  would change that answer, and nothing in the workflow assumes otherwise.

## See also

- [testing-architecture.md §10](testing-architecture.md) — the coverage banner and
  `--olo-require-vulkan`.
- [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md) — the same lesson from
  the other direction: check an extension's claims against the pinned SDK rather than a summary.
- [ADR 0010](../adr/0010-vulkan-rhi-heap-bindless-only.md) — the capability contract this rule is measuring drivers against.
