# Terrain collision for streamed tiles + sculpt edits (issue #469)

Follow-up to #428 / PR #468, which gave **single-tile** terrain a static
`JPH::HeightFieldShape` body built once at `OnPhysics3DStart`. This slice closed
the two gaps that PR deferred. Read this before touching terrain × physics again.

## The two paths are separate — don't conflate them

- **Streaming** = many bodies, one per loaded tile, *full-shape create on load /
  destroy on evict*. Streamed tiles are file-loaded and immutable; they are never
  sculpted. No `SetHeights`.
- **Sculpt / erosion** = one single-tile body, *mutated in place* over the dirty
  rect via `HeightFieldShape::SetHeights`. Single-tile terrain only.

`Scene::UpdateTerrainCollisionAfterEdit` early-returns for `m_StreamingEnabled`;
the streaming reconcile early-returns into its own tile map. Keep it that way.

## Streaming per-tile bodies live in a SECOND JoltScene map

`JoltScene::m_TerrainBodies` (keyed by terrain entity UUID, from #428) is for the
single-tile body. Streamed tiles use `m_TerrainTileBodies`, keyed by
`(terrain entity UUID, tile grid X, tile grid Z)`. Both are raw static
`NON_MOVING` bodies with `UserData = the OWNING TERRAIN ENTITY UUID` (so a raycast
against *any* tile resolves the terrain entity) and both are entered in
`m_BodyIDToEntity`. `DestroyAllTerrainBodies` (Shutdown), `OnPhysics3DStop`, and
the `DestroyEntity` hook must sweep **both** maps — a tile body has no
`m_RuntimeCollisionBodyToken`, so the token-guarded single-tile teardown misses it.

## The streamer only ticks in the render path — so does the reconcile

`TerrainStreamer::Update` (and therefore the per-tile collision reconcile,
`ReconcileStreamingTerrainCollision`) runs inside `Scene::ProcessScene3DSharedLogic`,
which is called **only** from `RenderScene3D`, **not** from the headless
`OnUpdateRuntime` / `SimulateRuntimeStep` tick. `TerrainTile::BuildGPUResources`
hard-requires a GL context (no null-guard), and the streamer only inserts a tile
into `m_Tiles` *after* that build — so **streamed-terrain collision cannot be
driven in a headless test**. The tile's *CPU* height field (`GetTerrainData()`) is
built GPU-free in the async `LoadFromFile`/`CreateFlat`, so the collision shape
itself needs no GL — but you cannot get a tile to `Ready` without one.

Consequence for tests: exercise the **collision primitives** the reconcile calls
(`JoltScene::CreateTerrainTileBody` / `DestroyTerrainTileBody`,
`Scene::UpdateTerrainCollisionAfterEdit`) directly from CPU heights, and verify the
full streamer→collision + editor-sculpt integration in the running editor. See
`OloEngine/tests/Functional/AnimationPhysics/TerrainStreamingSculptCollisionTest.cpp`.

## Tile body pose must reproduce the draw transform exactly

The draw path places each tile at `tileModel = entityTransform *
translate(tile.WorldOrigin)` (`submitChunkPackets`). The shape bakes entity scale
(`CreateTerrainHeightFieldShape(..., transform.Scale)`), so the *body* carries only
translation + rotation:

    bodyPos = transform.Translation + rotation * (transform.Scale * tile.WorldOrigin)
    bodyRot = transform.GetRotation()

For the common identity-rotation / unit-scale terrain this is just
`Translation + WorldOrigin`. Get the scale/rotation wrong and collision silently
drifts off the visible tile.

## `HeightFieldShape::SetHeights` footguns (the sculpt path)

1. **Block alignment is mandatory.** `inX`, `inY`, `inSizeX`, `inSizeY` must all be
   multiples of the shape's `GetBlockSize()` (default 2). Snap the dirty rect
   **outward** (`x0 = (x/bs)*bs`, `x1 = ceil` to `bs`), clamp `x1`/`z1` to
   `GetSampleCount()`. Get this wrong and Jolt asserts (debug) or writes garbage
   (release) — silent.
2. **Odd resolution → padded shape.** `CreateTerrainHeightFieldShape` pads an odd
   resolution up one edge-replicated row/col, so `GetSampleCount()` (e.g. 66) can
   exceed `TerrainData` resolution (65). Do **not** hand `SetHeights` a strided
   pointer into the resolution-wide `TerrainData`: build a **contiguous,
   edge-replicated** region buffer (`src = fullHeights[min(z,res-1)*res +
   min(x,res-1)]`), mirroring exactly how the shape was first filled. Then stride =
   region width.
3. **`SetHeights`/`GetHeights` take WORLD-space Y, not the normalized `[0,1]`
   samples.** Internally `SetHeights` re-quantizes via `(h - mOffset.Y) / mScale.Y`
   (HeightFieldShape.cpp), and `GetMinHeightValue()`/`GetMaxHeightValue()` return the
   world band. `TerrainData` heights are normalized, so convert:
   `worldH = GetMinHeightValue() + normalized * (GetMaxHeightValue() - GetMinHeightValue())`.
   Passing the raw `[0,1]` value writes a near-zero ridge that reads back as flat.
4. **A near-flat build has ZERO sculpt headroom unless you pin the range.** Jolt's
   `HeightFieldShapeSettings` defaults `mMinHeightValue`/`mMaxHeightValue` to
   ±`cLargeFloat`, so `DetermineMinAndMaxSample` fits the encodable 16-bit window to
   the *initial* samples. A body built from a flat (or low-relief) field then quantizes
   over a near-zero vertical range, and a later `SetHeights` raise silently clamps back
   to flat — collision never follows the sculpt. `CreateTerrainHeightFieldShape` pins
   `mMinHeightValue = 0`, `mMaxHeightValue = 1` (the full normalized band → world
   `[0, joltScale.Y]`) so **every** terrain body — flat or not — can be sculpted across
   its whole height range. Sub-mm precision cost. This was the bug that made the sculpt
   tests read flat after a raise even though `SetHeights` "succeeded".
5. **`NotifyShapeChanged` deadlocks under a body lock.** It takes the body lock
   itself. Do NOT wrap `SetHeights` + `NotifyShapeChanged` in a `BodyLockWrite` on
   the same body — fetch the shape via `BodyInterface::GetShape` (const_cast is safe:
   the shape is freshly built per terrain body, never shared), `SetHeights`, then
   `NotifyShapeChanged(bodyID, prevCOM, false, DontActivate)` with `prevCOM` read
   **before** the mutation. The "no concurrent query" guarantee comes from the
   caller running on the game thread outside the physics step, not from a lock.
6. **Debounce.** Update collision once per **stroke settle** (mouse-up / erosion
   apply button), never per drag frame — the editor wires it at the stroke-end /
   erosion-apply sites next to `RebuildDirtyChunks`, not through
   `terrain.m_NeedsRebuild` (that is a *full* procedural-rebuild flag).
