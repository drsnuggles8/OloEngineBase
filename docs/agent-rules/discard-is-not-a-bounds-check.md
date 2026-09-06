# `discard` is not a bounds check

**In a fragment shader, bound a GPU-driven index by clamping it, not by
`discard`ing above it.** `discard` stops the fragment being *written*; it does
not stop the invocation continuing, so a load that depends on the index is still
issued. Under the root-data model every storage buffer reaches the shader as a
bare `VkDeviceAddress` with no length (ADR 0011 §4), so that load has no bounds:
it is a device fault, not a wrong pixel.

```glsl
// WRONG — the load below still happens for a rejected index
if (i >= count)
    discard;
Record r = records[i];

// RIGHT — the address can no longer be formed out of range
if (i >= count)
    discard;
r = records[min(i, max(count, 1u) - 1u)];
```

The same reasoning does **not** apply to compute: `return` from a compute
`main()` genuinely terminates the invocation, so a guard there only needs the
right bound, not a clamp.

## The story (#1058)

`VirtualVisibilityResolve.glsl` resolves the software rasterizer's visibility
buffer. It walks a chain in which every index is read out of a GPU-written
buffer and addresses the *next* buffer:

```
pixels[] → swRecordIndex → swList.Records[] → ClusterIndex → clusters[] → IndexBase
        → localIndices[] → cluster-local index → vertices[]
```

Every step was guarded — and every guard was a `discard`. The pass device-faulted
with `VK_EXT_device_fault` reporting a READ of invalid address, 3/3.

**The data was fine.** A CPU readback of one frame gave `swList.Count` 604
against a capacity of 4860, and across all 604 records: zero bad instance
indices, zero bad cluster indices, zero index-arena overruns, and zero
cluster-local indices outside their cluster's vertex window — steady across 340
frames. Chasing "which index is corrupt" was the wrong question for most of a
session.

What made it certain rather than rare was the **sentinel**. A cleared
visibility-buffer pixel is all-ones, and the payload decodes as
`swRecordIndex = packedPixel.x >> 9`, so a cleared pixel asks for record
**0x7FFFFF** — 8.4 million records, ~134 MB past the allocation. On any frame
most of the screen is cleared, so the out-of-range index is not an edge case; it
is the common case, arriving on nearly every invocation.

## Why nobody had noticed

That draw had **never executed on Vulkan**. The pass zeroes the SW work list's
16-byte header mid-frame, and the command-ordered snapshot mechanism turned that
into a 16-byte stand-in for the whole buffer, so the resolve read `Count == 0`
and discarded every pixel. Fixing the snapshot
([vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md))
ran the shader for the first time. Two bugs in series, the first hiding the
second.

## Three traps this cost time on

- **`discard` makes bisecting unreliable.** Moving a `discard` up and down the
  shader to find the faulting line gives answers that contradict each other,
  because the thing you are moving is exactly the thing that does not stop
  execution. Bisect with a **constant write plus `return`** instead, and the
  partition becomes trustworthy.
- **Blaming the arm named in the issue.** The fault was predicted for the
  mesh-shader raster arm. The three-cell matrix settled it in three runs:
  `forcemdi` faulted identically (exonerating the mesh arm) and
  `swRasterMode=disabled` was stable. Build the matrix before bisecting a shader.
- **Repeated device losses are not a diagnostic method.** Once you can make the
  fault happen, stop making it happen. Disable the faulting draw and read the
  buffers back on the CPU: that is what produced the numbers above, in one run,
  with no device loss.

## Bound by capacity, not by a GPU-written count

Where a shader has both, bound the index by the **CPU-known capacity**, not by
the list's own count. `VirtualClusterCull.comp` appends with

```glsl
uint swSlot = atomicAdd(swList.Count, 1);
if (swSlot < u_SwCapacity) { /* write */ }
```

Issue #862 bounded the **append**; the counter still increments past capacity by
design, so an overflow drops the record *and* inflates the number a reader would
bound itself by. Both consumers now carry the capacity as a uniform
(`u_VirtualSwListCapacity` in `VirtualDrawInfo`, `u_SwListCapacity` in
`VirtualRasterParams`).

Finally: **write every clamp underflow-safe.** `count - 1u` wraps to 0xFFFFFFFF
when `count` is 0, which reintroduces precisely the bug being fixed. Spell it
`max(count, 1u) - 1u`.
