# Attribute a validation message to a layer configuration before you change engine code

Before treating a validation-layer message as an engine defect, **A/B it against the layer
settings that produced it**. GPU-assisted validation adds usage bits to the application's buffers
so its own shaders can read them, and core validation then judges the application against the
*modified* create info. The message names your buffer and your parameters, and neither is wrong.

The general rule: a message is evidence about `(engine, layer configuration)`, not about the
engine alone. Reproduce it with the configuration stated, and re-run with the suspected layer
feature off, before writing a fix.

## The rules

**State the layer configuration whenever you report a validation message.** "Ten errors per run
in the Debug editor" is not reproducible; "ten errors with `khronos_validation.gpuav_enable=true`"
is. A message with no configuration attached costs the next person the whole A/B.

**Run the A/B matrix before diagnosing.** It takes one short test run per row:

```bash
cd OloEditor
VK_LAYER_SETTINGS_PATH=<file> ../build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe \
  --gtest_filter='VulkanDrawPath.FacadeDrawRendersTintedTextureThroughRootData'
```

**Prove the layer is actually on before citing silence.** A clean run is only evidence if the
layer was loaded and read your settings file. The cheap positive control is
`khronos_validation.validate_best_practices = true`: it warns about almost any application, so if
those warnings do not appear, the settings file is not being read and every other row is
worthless.

**A per-command flag mirroring a create-time bit cannot be "made quiet".** The two-directional
pair (`VUID-*-13122` requires the flag, `VUID-*-13123` forbids it) has no value that satisfies
both for a buffer whose usage you got right. Silencing one direction buys the other. The only
blanket-legal answer is the `UNKNOWN_` variant, which is a real cost: it tells the driver less on
every command that carries it, forever, to work around a debug tool.

## What this cost, once (#1200, 2026-09-14)

An issue reported `vkCmdBindIndexBuffer3KHR` binding a STORAGE-usage index buffer with
`VkAddressCommandFlagsKHR(0)`, ten times per run, inside `ScenePass`, `ParticlePass` and
`AOApplyPass`. It came with a diagnosis (the #1171 per-draw vertex-stream snapshot substitutes an
address after resolution) and a fix (carry the storage flag on the raw family's binding).

Every part of that was wrong, and the code said so before any hardware was involved:

- The raw family **already** carries `VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR`; the binds
  reporting flags 0 were the *object-backed* family, where VUID-13123 **forbids** it.
- The snapshot path cannot reach an index bind: only two buffer families in the engine are created
  with `INDEX_BUFFER_BIT`, and the frame arena is neither. An arena address bound as an index
  buffer would have tripped VUID-13051 as well, which the report did not show.

The measured A/B, with the engine unchanged:

| layer configuration | VUID-13122 |
|---|---|
| default Debug (core + sync) | not reported |
| `validate_best_practices = true` | not reported (and best-practice warnings prove the layer is live) |
| `gpuav_enable = true` | **reported** |
| `gpuav_enable` + `gpuav_index_buffers = false` | reported |
| `gpuav_enable` + `gpuav_shader_instrumentation = false` | reported |
| `gpuav_enable` + `gpuav_buffers_validation = false` | not reported |
| `gpuav_enable` + `gpuav_debug_disable_all = true` | not reported |

The buffer the layer named was matched to its creation by a trace of every index buffer's address
range and handle: it was a `VulkanIndexBuffer`, created `INDEX|TRANSFER_DST|TRANSFER_SRC|
SHADER_DEVICE_ADDRESS` with no storage bit anywhere. GPU-AV's **buffer validation** family — the
one that reads indirect-draw arguments, copies, index buffers and acceleration-structure build
inputs on the GPU — adds `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` to application buffers so its
validation shaders can address them. Core validation then reads that usage back and reports the
application for not declaring a bit the application never asked for.

So there is nothing to fix in the backend. The named fix was also **measured** rather than argued
about: applying it (object-backed arm -> `StorageUsage::Present`) and re-running the new test
reports `VUID-VkBindIndexBuffer3InfoKHR-addressRange-13123` under the **default** configuration,
with no GPU-AV anywhere. It trades an artifact that appears only under a debug tool for a real
violation on every ordinary run. What the episode left behind instead:

- `OLO_VK_TRACE_BUFFERS` now also logs the index **bind** (address range, `VkBuffer`, resolved
  `addressFlags`) alongside the create-time ranges. Matching a layer message's
  `belongs to VkBuffer 0x…` to a creation took minutes with it and was guesswork without it.
- `VulkanDrawPath.IndexBindAddressFlagsFollowEachFamilysCreateTimeStorageUsage` asserts the
  decision for **both** families, so the direction of travel this issue proposed fails a test
  rather than a hardware run someone has to notice.

**If you want a quiet GPU-AV run on this engine**, set `khronos_validation.gpuav_buffers_validation
= false`; you keep shader instrumentation and lose the buffer-content checks. Do not change the
backend's `addressFlags` to achieve it.

See [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md) for the sibling
rule about the same extension: check the claim against `validusage.json` before building on it.
