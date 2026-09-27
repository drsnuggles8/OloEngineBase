# Resolving a Vulkan device fault: address, pass, and now owner

**Rule:** a `VK_EXT_device_fault` address means nothing on its own. Three extensions answer three
different questions, and a fault investigation should turn all three on before forming a single
hypothesis:

| Question | Mechanism | How |
|---|---|---|
| *What address faulted?* | `VK_EXT_device_fault` | always on when the driver offers it |
| *Which pass, on which queue?* | `VK_NV_device_diagnostic_checkpoints` | always on (NVIDIA), issue #1201 |
| *What object CLASS owned that address?* | `VK_EXT_device_address_binding_report` | `OLO_VULKAN_ADDRESS_BINDING_REPORT=1`, issue #1198 |
| *Which resource exactly, was it already destroyed, and which shader read it?* | **Nsight Aftermath** | `OLO_VULKAN_AFTERMATH=1`, issue #1198 |

The third is new and it is **off by default**: the validation layer implements it by emitting a
debug-messenger message on every allocation bind and unbind, so it costs on every allocation. Turn
it on for a fault investigation, not for a normal session. It is Debug-only — the validation layer
is what implements it.

With all three, a report reads:

```
[Vulkan] DEVICE FAULT REPORT: '' (2 address record(s), 0 vendor record(s))
[Vulkan]   fault address: 0x200720ee0 (precision ±0x10) — instruction pointer (faulting)
[Vulkan]     owner: no recorded binding covers 0x200720ee0 (1043 ranges tracked)
[Vulkan]   fault address: 0x3e2000000 (precision ±0x1000) — READ of invalid address
[Vulkan]     owner: UNBOUND (freed) VkImage handle=0x2bb01faaed0 range=0x3e2000000..0x3e2b10000 (seq 977)
[Vulkan]     owner: bound         VkImage handle=0x2bb01faaed0 range=0x3e2000000..0x3e2b10000 (seq 681)
[Vulkan]   checkpoint (graphics): stage=0x1 pass='ScenePass'
[Vulkan]   checkpoint (async compute): stage=0x1 pass='VolumetricFogPass'
```

That is "a `VkImage` was bound at sequence 681, freed at 977, and read afterwards by ScenePass on
the graphics queue" instead of "READ of invalid address".

## Reading the owner lines

- **Newest first.** A GPU address range is reused constantly; the most recent record is the one in
  force at the fault. An older `bound` line for the same range is history, not the answer.
- **An instruction-pointer record usually has no owner.** Shader code does not come from a reported
  binding, so "no recorded binding covers" on the IP line is expected, not a failure.
- **Both a `VkImage` and its `VkDeviceMemory`** appearing over an identical range means a dedicated
  allocation. Two *different* handles over overlapping ranges means VMA recycled the address.
- **"No recorded binding covers" on the READ line** means the address was computed, not freed. Look for the shader that built it from bad inputs, not for a lifetime bug. #1437 was a shader reading another producer's uniform block ([shared-uniform-binding-bind-before-dispatch.md](shared-uniform-binding-bind-before-dispatch.md)).
- The reported address is only precise to `addressPrecision`, so the resolver matches the whole
  ± window. Several ranges can legitimately match.

## Two traps this cost time on (issue #1198)

**Handles are not comparable across the layer boundary.** The address-binding report carries the
*driver's* handle; engine-side logging of the same `VkImage` prints the *layer-wrapped* handle
(`unique_objects` gives non-dispatchable handles a synthetic value — it looks like
`0x1360000000136`, an obvious `(n << 32) | n`, not a pointer). Correlating an owner line with an
engine-side destroy log **by handle** silently finds nothing, which reads as "this object never
went through our destruction path". Correlate by address range instead.

**Allocation size is not image size.** The size in a binding range is the padded VMA allocation, so
one size covers a `D32_SFLOAT` and an `R16G16B16A16_SFLOAT` target of the same extent. It is not an
identity.

## Aftermath is the one that ends the argument

The layer's address-binding report gets as far as "a `VkImage` was bound here and unbound there".
It cannot say whether the read was ours or a driver read past a legitimate free, and it cannot name
the shader. Aftermath answers both, and it is the difference between a week of hypotheses and one
run:

```
[Aftermath] PAGE FAULT at 0x3f4000000 - READ (engine=1, client=4, 2 resource(s) named)
[Aftermath]   resource: apiHandle=0x1ff061212d0 ... 1411x942x1 mips=1 vkFormat=130
                        destroyed=true createTick=800405706 destroyTick=800412123
[Aftermath]   active shader: hash=0x3dc4a10b8749b9db type=5 internal=false
```

`destroyed=true` plus the Vulkan-only `MemoryFreed` residency says the GPU read a resource the CPU
had already destroyed. That is the fact; it is **not yet blame**. `type=5` is
`GFSDK_Aftermath_ShaderType_Fragment` and `client=4` is `GraphicsProcessingCluster`.

Three more things the dump holds, all of which the one-line decode above threw away (#1511):

- **`internal=true`** on the active shader means a driver-internal shader: the engine recorded a
  copy, clear, blit or resolve, not a draw.
- **The JSON report** (`CrashReports/*.json`, written by `OLO_VULKAN_AFTERMATH=1`) maps the
  faulted warp to a source line, e.g. `PBR_MultiLight.glsl:7137`. The line number is in the
  preprocessed source, with includes inlined.
- **The automatic markers carry CPU call stacks.** `nv-aftermath-format -j -p <dir holding
  OloEditor.pdb> <dump>` resolves them. The `NotStarted` / `Executing` entry is the command in
  flight, named down to the engine function that recorded it.

Requires a build configured with `AFTERMATH_SDK_ROOT` (the SDK is licensed, so it is resolved from
the environment and never vendored — the Steamworks rule). Without it the lever warns rather than
going quiet. Arming happens before `vkCreateDevice`, so it is restart-required.

**One correlation trap.** Aftermath reports the resource by its RAW DRIVER `VkImage` handle, and so
does the address-binding report. Engine-side logging of the same image prints the LAYER-WRAPPED
handle whenever validation is on. Matching them by handle silently finds nothing, which reads as
"this resource never went through our destruction path" — a false and expensive conclusion. Either
correlate on the address range, or run the capture with `VK_LOADER_LAYERS_DISABLE=*` so the two
namespaces coincide.

## When every reference is absent: dump the descriptor heap before blaming the driver

This section used to end in "treat it as the driver's", and on #1198 that was wrong. #1198 and
#1504 each audited every engine reference on faulting runs: descriptor writes, every image-bearing
command, the command buffers submitted after the free, and a `vkDeviceWaitIdle` before it. All came
back empty, correctly, because the stale reference was not the engine's command. It was a descriptor
the DRIVER had written into the reserved range of the engine's own resource heap, for an earlier copy
of the then-current target. A later copy, recorded after `vkCmdExecuteCommands` had left the
primary's heaps unbound, read it. The rule and the evidence are in
[vulkan-descriptor-heap-rebind-after-execute-commands.md](vulkan-descriptor-heap-rebind-after-execute-commands.md).

So the audit has one more step, and it comes before any "driver bookkeeping" theory. A device loss
writes `CrashReports/descriptor-heap-*.bin` when the resource heap is still mapped and the file can
be written; otherwise the log says `no descriptor-heap dump for this fault` and names the reason.
Decode the reserved range and check whether an entry's image covers the faulting address. With no
dump, rerun the repro until one is written; do not fall back to the driver theory.

Two disciplines from the same investigations still hold. First, a fault that reproduces 1 run in 3
makes a single clean run worthless: replay every arm N>=8, interleaved. Second, run a destroy-side
audit at enqueue or at the top of `DestroyEntry`, not after the destroy pass has tidied up.

Related: [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md) — the async batch is what
submits a pass's command buffer mid-frame, which is what puts GPU execution and CPU recording of
the same frame in flight together.

A second fault of this shape (#1504) needed parallel recording and was set up by the shadow-cascade region, not the pass its checkpoint named; the audit above plus a per-region inline bisect is in [vulkan-parallel-cascade-recording-fault.md](vulkan-parallel-cascade-recording-fault.md).
