# No silent fallbacks

Applies to: the whole engine, and to review of any diff.

**A path that cannot do what it was asked must say so — loudly, and into something a test can
assert on. It must not substitute a plausible default and continue.** A fallback that is genuinely
needed for safety is kept *and* made loud; those are not alternatives.

The shape to reject, wherever it appears:

```cpp
if (!resolved)
{
    SetSomething(binding, nullptr);   // plausible default
    return;                           // no warn, no counter, no assert
}
```

## Why: two reasonable fallbacks composed into a device loss

Issue #1052. `VulkanRendererAPI::BindStorageBuffer` could not resolve a raw-buffer handle, so it
bound null and returned — silently. The publication site then found the binding unoccupied and
substituted the frame arena's zero-filled **null block**, which is itself a *deliberate* fallback
with a good reason: handing a shader address 0 is a page fault that escalates to
`VK_ERROR_DEVICE_LOST`, and that had already happened once.

Each decision is defensible alone. Composed, they turned "this entry point is not lowered yet" into
`vkQueueSubmit2` failing with `VK_ERROR_DEVICE_LOST` on **every** virtual-geometry scene on Vulkan —
and the failure surfaced three layers from its cause, as a `READ of invalid address` at a hardware
fault address. Two Vulkan virtual-geometry tests passed throughout.

## Rank a fallback by whether the substituted value can be INDEXED

This is the part that decides severity, and it is not obvious:

- An unfed **UBO** reads deterministic zeros. Defined, usually wrong-looking, survivable.
- An unfed **SSBO** is a *small* block that a shader **indexes** — and the index normally comes from
  a **different buffer that was fed correctly**. The read lands outside the stand-in and loses the
  device.

That asymmetry is the whole of #1052: the cluster records (binding 33) were fed and supplied a real
`cluster.IndexBase` in the millions, while the index arena (binding 42) was the null block.

**So: an unfed SSBO is safe only while everything that indexes it is also unfed.** Measured on this
engine — a Vulkan sweep of six scenes plus the virtual-geometry scenes found 21 distinct unfed
bindings, of which 7 are SSBOs (the Forward+ light lists, a lightmap vertex-pull buffer, the terrain
VT feedback buffer). All 7 are currently harmless *by accident*: they are unfed as a **group**, so
the count that drives the loop reads 0 out of the same null block and the indexing never happens:

```glsl
uvec2 tileData = fplusGetTileData(viewDepth);  // fplusGrid[...] -> null block -> (0, 0)
uint count = tileData.y;                       // 0
for (uint i = 0u; i < count; ++i)              // never runs
    fplusLightIndices[offset + i];             // never reached
```

Feed one member of such a group without feeding its siblings and it detonates. Do not read "no
crash today" as "safe".

## The counter-moves

- **Warn once, and count.** The engine already has the shape:
  `UnimplementedStub(entry, StubKind::PreconditionFailure)` warns once per entry point and
  increments a tally, and `VulkanDrawPathTest` asserts `GetUnimplementedStubHitCount() == 0` — "the
  draw path must not fall through to a stub". A silent `return;` after a failed lookup is the bug,
  not the house style.
- **Give the dangerous case its own voice and its own counter.** An unfed *storage* binding now logs
  at ERROR and increments `GetUnfedStorageBindingCount()`, separate from the stub tally so existing
  assertions keep meaning what they meant. A tenant that renders real geometry should assert it is
  zero.
- **Assert the counter in tenants, not just in the one test that already does.** That check is
  *backend-shaped* rather than feature-shaped: it catches the next unlowered path in whatever pass
  reaches it first, which is exactly what did not happen here.
- **In review, treat `if (!x) { setDefault(); return; }` with no diagnostic as a finding**, the same
  way a swallowed exception would be. Ask what the caller does next with the default.

## What stayed green

Two Vulkan virtual-geometry tests, both passing, for as long as the bug existed — they hand-author
the cluster set and build the failing buffer a different way (see
[substituted-seams-compound.md](substituted-seams-compound.md), second instance). The stub counter
that would have caught it exists and is asserted — but only on the draw path, never on a
virtual-geometry tenant. And `RendererAttachedTest`, which every virtual-geometry evidence test
rides, creates a GL context only, so the real pass had never executed on Vulkan in any test.

Found on #1052 / PR #1054.

## Related

- [substituted-seams-compound.md](substituted-seams-compound.md) — every substitution a test makes
  is a seam it stops testing, including building the same object a different way.
- [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md) — the other
  family of silent state loss.
- [live-verification-noise-floor.md](live-verification-noise-floor.md) — a green result that proves
  nothing is worse than a red one.
