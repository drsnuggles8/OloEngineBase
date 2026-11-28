# A shader's local memory must fit what the NVIDIA driver backs

**Keep every shader's per-thread local memory (its spilled stack) below
2^32 B ÷ (SM count × resident threads per SM): 21 845 B on an RTX 4090.**
NVIDIA driver 621.14 creates a pipeline whose frame is larger without an error,
and the first spill writes to unbacked memory and loses the device. Measure the
frame instead of guessing it: run with `OLO_VK_PIPELINE_STATS=1` and read the
`[Vulkan/PSO stats]` line's `Local Memory Size`, minus the 2^36 the driver adds
(a stage with no local memory reports exactly 2^36). The budget needs
`VK_NV_shader_sm_builtins`; `VulkanDevice::GetShaderSMCount` and
`GetShaderWarpsPerSM` expose it while the lever is on.

**Merge call sites before cutting work.** The frame is static: it is the
maximum over every inlined call path, executed or not, so a branch that never
runs still counts. Two call sites of one large function each inline their own
temporaries. Route them through one call in a `[[dont_unroll]]` loop, with the
inputs selected before the call (a ternary between two calls is still two call
sites).

## What happened (#1807)

The ReSTIR PT replay-device suite lost the RTX 4090 on its first drawing case.
Windows logged `Graphics SM Warp Exception … MMU Fault`, and
`VK_EXT_device_fault` named a **write** to an invalid address with no engine
buffer near it. The investigation narrowed it in this order:

- Disabling the counter atomics, and then every explicit store, left the fault
  in place, so the write was not an application store.
- Returning before the transport body removed it (the attachments were fine).
- Compiling out the reuse branch removed it, although the failing stage-0 draw
  never executes that branch. A fault that depends on code that does not run
  points at a static per-shader resource.
- Pipeline statistics: the fragment shader used 255 registers and **21 984 B** of
  local memory per thread. The variant without the reuse branch used 3 216 B.
- The variant without the reuse branch was padded with a dynamically indexed
  local array. At 21 776 B it ran; at 21 904 B it faulted with the same write.
  2^32 ÷ (128 SMs × 48 warps × 32 threads) = 21 845 B.

`PTCombine` called `PTShift` twice: once for the forward map and once more inside
its MIS helper. Each call inlined a full `PTMapOne` with several 592-byte
`PTPath` temporaries. One call site brought the frame down to 15 008 B. The
results are unchanged: the replay, energy and reference-integral cases pass on
the 4090.

Rebinding the descriptor heaps after the mid-recording pipeline creation did not
help, which rules out a stale driver descriptor in the heap's reserved range.
AMD radeonsi and lavapipe never faulted, so a passing AMD or lavapipe run says
nothing about this limit.

`ReSTIRPTDeviceStatistics.ReplayShaderLocalMemoryFitsWhatTheDriverBacks` checks
the frame against the budget on any NVIDIA device and skips elsewhere.
