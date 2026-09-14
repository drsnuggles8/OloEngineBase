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
had already destroyed. That is the fact; it is **not yet blame**. Whether the stale reference is the
engine's or the driver's needs the audit in the next section — on #1198 the same signal turned out
to be a driver-side read once every engine-side reference had been shown absent.
`type=5` is `GFSDK_Aftermath_ShaderType_Fragment` and `client=4` is `GraphicsProcessingCluster`, so
the read is a texture fetch in a fragment shader rather than a depth-test or copy.

Requires a build configured with `AFTERMATH_SDK_ROOT` (the SDK is licensed, so it is resolved from
the environment and never vendored — the Steamworks rule). Without it the lever warns rather than
going quiet. Arming happens before `vkCreateDevice`, so it is restart-required.

**One correlation trap.** Aftermath reports the resource by its RAW DRIVER `VkImage` handle, and so
does the address-binding report. Engine-side logging of the same image prints the LAYER-WRAPPED
handle whenever validation is on. Matching them by handle silently finds nothing, which reads as
"this resource never went through our destruction path" — a false and expensive conclusion. Either
correlate on the address range, or run the capture with `VK_LOADER_LAYERS_DISABLE=*` so the two
namespaces coincide.

## When every reference is absent: audit by interposition, then treat it as the driver's

#1198's fault survived every engine-side fix because the read was not the engine's. The way to
establish that — rather than assert it — is to interpose the API and audit the *data*, in this
order, each on a faulting run:

1. **Was a descriptor ever written for the image?** Shadow every `vkWriteResourceDescriptorsEXT`
   at its single funnel. If the answer is never, no stale-descriptor theory can be right, cached or
   not.
2. **Does any command submitted after the destroy reference it?** Interpose the volk pointers for
   every image-bearing command (barriers, copies, blits, clears, resolves, `vkCmdBeginRendering`
   through a view→image map built from `vkCreateImageView`) and log from the trigger onward.
3. **Was any command buffer submitted after the trigger recorded before it?** Hook
   `vkBeginCommandBuffer` / `vkQueueSubmit2` / `vkCmdExecuteCommands` and compare frames — a stale
   secondary or parked primary carries the old address inside its command data.
4. **Is the reader in flight at the free?** `vkDeviceWaitIdle` immediately before the free. If the
   fault survives that, the reader is submitted afterwards.

On #1198 all four came back empty on faulting runs, with Aftermath reporting an address-translation
fault at the destroyed depth attachment's exact base address, the scene fragment shader active. A
read that no submitted command asks for is the driver's per-surface bookkeeping for a depth target
(hierarchical-Z / compression metadata is keyed by the surface base), consulted for the first
rendering frame after a same-spec replacement. The engine-side response is
`VulkanDeferredReclaim::kDepthStencilHoldGenerations`: depth-stencil images outlive their last use
by one generation more than everything else, behind `OLO_VULKAN_NO_DEPTH_RECLAIM_HOLD` so it can be
re-tested against a new driver. Two traps on the way: a `WasDestroyed` set keyed on raw driver
handles false-positives the moment the driver recycles a handle value (it does, immediately), and
an audit that runs *after* the destroy pass's own cleanup measures the tidied-up state and reports
zero — run it at the top of `DestroyEntry`, or at enqueue.

One more discipline the same issue paid for: a fault that reproduces roughly 1 run in 3 makes a
single clean run worthless as evidence. Replay every arm N>=8 before believing it, and interleave
the arms rather than running them in blocks. Three consecutive clean runs on #1198's own base
looked like "already fixed" and were not.

Related: [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md) — the async batch is what
submits a pass's command buffer mid-frame, which is what puts GPU execution and CPU recording of
the same frame in flight together.
