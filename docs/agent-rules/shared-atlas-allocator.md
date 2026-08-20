# Shared atlas allocator (#718) — what it is, what it isn't, and a non-RAII leak trap

`OloEngine::AtlasAllocator` (`Renderer/AtlasAllocator.h`) is a reusable power-of-two
square-region allocator: allocate a 2^k tile, free it, query occupancy. It's a fixed
quadtree stored as a flat "4-ary heap" (node 0 = the whole atlas; the 4 children of node
`i` live at `4i+1..4i+4`), modelled on the MIT-licensed `AtlasAllocatorPD2.cs`
(Hollow-TerrainSystem) minus its Morton/BMI2 fast path. That fast path exists to skip
whole allocated subtrees in O(1) so a scan stays cheap however often `Allocate()` runs;
this class instead uses a plain O(nodes-at-that-level) linear scan, which is cheap in
practice **because neither current instantiation calls `Allocate()` on a hot path** — a
shadow-atlas repack only happens when a caster's rank/tier changes, and an impostor bake
only when a foliage layer's mesh/params change. The node count itself is NOT uniformly
small: the shadow atlas's table stays in the low thousands, but the impostor VRAM
budget's (8192px atlas / 32px min tile) has ~87k total nodes, ~65k of them at the finest
level alone — still fine for a rare, non-hot-path call, but a future instantiation with a
genuinely hot `Allocate()` path and a large/fine-grained table should measure rather than
assume this scan is still free. Coalescing isn't a separate code path either way: because
the whole tree exists up front, freeing every child of a node just makes that node itself
allocatable again.

Two retrofits ship in the same PR, at deliberately different depths of commitment.

## ShadowAtlas: the free function is untouched; production code moved to a new stateful sibling

`ShadowAtlas::Allocate()` — the pure, per-call rank-and-shelf-pack function
`ShadowAtlasPackingTest.cpp` exercises — is **unchanged**. None of its tests assert exact
tile *coordinates*, only sizes/ranks/counts/overlap/determinism, which is what made it
safe to leave it backed by the original shelf packer while building something new
alongside it.

Production code (`Scene.cpp`'s per-frame shadow setup) now calls
`ShadowMap::AllocateAtlasTiles()`, a thin forwarder to a `ShadowAtlas::PersistentAllocator`
member that owns an `AtlasAllocator` across frames instead of building a fresh packer every
call. A caster is recognised across frames by `Candidate::UserData` (the light entity's
UUID, widened from `u32` to `u64` — a truncated identity would alias unrelated lights into
"reusing" each other's tiles); a caster with `UserData == 0` is always treated as new. The
ranking/tiering/budget loop is **duplicated** rather than shared with the free function,
because the two backends can fail to place a candidate at different points (shelf-packer
exhaustion vs. buddy-allocator fragmentation), which entangles "which rank got which size"
with "did placement succeed" — see `PersistentAllocator::Allocate`'s comment for why this
couldn't cleanly factor into rank-then-place stages.

**Freeing a departed caster's tile still lags one call in the worst case.** A caster no
longer present as a candidate at all (removed, stopped casting) is freed EAGERLY, before
ranking runs — a cheap upfront pass over the held set catches this common case so a
legitimate allocation ranked ahead of it in the SAME call isn't blocked by space that
caster no longer needs. A caster that IS still a candidate but whose rank crossed a tier
boundary (large → medium, say) is *also* freed eagerly, the instant the size mismatch is
detected, rather than left holding both its old and new tile until the trailing sweep —
that specific case IS knowable in advance (the mismatch is checked right before the
candidate's own allocation attempt), unlike the one case that genuinely isn't: a caster
that IS still a candidate this call but doesn't make the accepted set at all (out-ranked,
over budget). Whether THAT candidate is accepted is only knowable after the ranking loop
actually runs, so its tile frees on the trailing sweep and becomes available NEXT call,
not this one — under near-capacity atlas
conditions this can reject a candidate the old from-scratch shelf-packer would have
placed. Deliberate, not fixed: the atlas is sized with generous headroom relative to the
entry/light budgets specifically so this stays rare, and a real fix is a two-pass "which
rejections this call would free enough space to accept something else" solver, which is
more machinery than a one-call lag under a rare, already-budgeted-for condition earns.

**The swap-not-mutate pattern for identity-based free-on-drop.** The first draft of
`PersistentAllocator::Allocate` mutated its `m_Live` vector in place while iterating: push
a newly-accepted candidate's slot, then after the loop, sweep `m_Live` and free anything not
in this frame's `kept` list. Tracing it all the way through, that draft turned out **not**
to be an actual bug for this call shape (the sweep runs strictly after the ranking loop
finishes, so nothing later in the *same* call could grab a just-freed node) — but it's the
kind of thing that stops being safe the moment a future change interleaves placement with
cleanup, and it's fragile to reason about even now. The shipped version swaps
`m_Live` into a local `previousLive` at the top of the call, builds the new `m_Live` fresh
from this call's accepted set (removing a slot from `previousLive` when it's reused), and
frees whatever is left in `previousLive` in one pass at the end. A slot can now only ever
leave `m_Live` by being reused into the new one or freed in the trailing sweep — never both,
and never a slot the same call just added. If you're writing another identity-keyed
allocate/free-on-drop cache, use this shape, not "iterate the live set, free what's not
kept" — the moment cleanup interleaves with insertion, the entangled version needs
re-deriving from scratch to prove it's still safe.

## Impostor atlas: a VRAM *budget*, deliberately not spatial packing

The issue's motivating text calls out "impostor baking rolls its own layout" as a problem
this issue should fix. Read literally that suggests packing every `FoliageLayer`'s
octahedral bake into sub-rects of one shared physical texture — but today each layer
already gets its own **dedicated** `Ref<Texture2D>` pair sized exactly to its own bake
(no cross-layer sharing exists to retrofit). Making that literal would mean:

1. `ImpostorBaker::Bake` render into an offset viewport of one shared persistent
   framebuffer instead of a scratch one, with the shared region's clear scoped to a
   scissor rect (so one layer's rebake can't blank a sibling's already-baked tile);
2. `FoliageRenderer` bind ONE shared texture per draw instead of a per-layer one, and pass
   a UV scale/offset into `Foliage_Impostor.glsl`'s tile-sampling math;
3. `ImpostorBakeEvidenceTest.cpp`'s `ConeBakesViewDependentOctahedralAtlas` rewritten
   entirely — it reads back `atlas.Albedo` assuming its *whole* width/height IS this
   bake's footprint (`size = atlas.Albedo->GetWidth()`, tile math `fx*tileRes+tx` with no
   region offset). A shared texture makes that assumption false, and every "did the bake
   render" / "is it view-dependent" assertion would need re-deriving against the new sub-rect.

That's exactly the shape [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md)
warns about: a UV/layout change to this exact shader is a three-time repeat offender for
"impostors render a plausible-but-wrong frame, and it takes multiple azimuths to notice."
Given the retrofit's actual value — bounding total impostor VRAM, which today has **no**
ceiling at all (any number of `FoliageLayer`s can each demand a full-resolution atlas pair)
— doing that risk didn't pay for itself in this PR.

What shipped instead: `ImpostorBaker` owns a static, process-wide `AtlasAllocator` used
purely as an **accounting budget**. `Bake()` must reserve a power-of-two region sized to
its own footprint before creating any GPU textures; on exhaustion it logs and returns an
invalid atlas (the same graceful-degradation path already used for a null mesh or missing
shader — `FoliageRenderer` already skips rendering an invalid impostor). Zero shader
changes, zero rendering-path changes, and the existing evidence test's assumptions all
still hold, because a *successful* reservation changes nothing about how the bake proceeds.

**If #715 slice 3 (or anything else) wants real spatial sharing for VT/impostor
textures later**, the allocator underneath is already the right tool — `AtlasAllocator`
doesn't know or care that this consumer only ever asked for accounting, not placement. The
three numbered risks above are the checklist for that work, not a reason it can't be done.

## The vector::resize non-RAII leak trap

`ImpostorAtlas.BudgetNode` is a plain `u32` handle into that static allocator — not a
`Ref<T>`, so nothing runs when an `ImpostorAtlas` is destroyed. `FoliageRenderer::m_Layers`
is a `std::vector<LayerRenderData>`, and `LayerRenderData` embeds an `ImpostorAtlas` by
value. `std::vector::resize()` to a *smaller* size silently destroys the trailing elements
— which, for every other member of `LayerRenderData` (`Ref<VertexArray>`,
`Ref<Texture2D>`, …), is exactly correct (refcounted GPU cleanup). For `BudgetNode` it's a
leak: the claim is never freed, and because the allocator is a process-wide static, it
leaks for the rest of the process — a scene-reload loop that keeps removing/re-adding
foliage layers would eventually starve every future bake. Five discard sites needed an
explicit `ImpostorBaker::Free(atlas)` that nothing in the type system would have forced:
the two `data.Impostor = ImpostorAtlas{}` resets, the rebake overwrite, the layer-count
shrink before `m_Layers.resize()`, and — the one that's easy to forget entirely —
`FoliageRenderer`'s own destructor (a `Ref<FoliageRenderer>` dropping to zero, e.g. an
entity removed or a scene closed, previously ran the implicit default destructor and never
touched the budget at all). Giving `ImpostorAtlas` a real destructor instead was considered
and rejected: a user-declared destructor suppresses the implicit move ctor/assign, so
`data.Impostor = ImpostorBaker::Bake(...)` (a genuine move-assignment, not elided the way
`ImpostorAtlas atlas = ImpostorBaker::Bake(...)` is) would fall back to copy semantics —
and a naive copy assignment double-frees the just-reserved `BudgetNode` the moment the
temporary's destructor runs. `AtlasAllocator::Free` being tolerant of a double-free (a
second `Free()` on an already-free node is a documented no-op, not a crash) is what makes
this class of mistake survivable rather than a live bug — but the five explicit discard
sites are still what makes it *correct*, not just non-crashing.

**The general lesson:** a plain accounting handle embedded in a value type that otherwise
looks entirely POD (glm vectors, bools, floats, `Ref<T>`s that clean up on their own) is
invisible at every call site that resets or replaces that value type — `= T{}`,
`vector::resize()`, `vector::erase()`, the enclosing object's own destructor if nobody
declared one. Grep for every site that discards a value of that type, not just the ones
that felt like "removal."
