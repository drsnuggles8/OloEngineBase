# Terrain voxel volumes: authored vs rebuildable, shared vs detached

**Rule.** A `TerrainComponent`'s voxel volume is shared by every copy of the component and detached
only where a copy becomes a separate world. Only an *authored* volume is persisted. Whether a volume
is authored is a property of the volume (`VoxelOverride::IsAutoSeeded`), not of the component.

## 1. Copies share; separate worlds detach

The copy constructor and `operator=` share `m_VoxelOverride`, exactly as they share `m_Material`.
`Scene::Copy`, `Scene::DuplicateEntity` and the prefab copy macros (`Prefab.cpp`) call
`DetachVoxelVolume()`, which clones authored content and drops a seeded volume so it re-seeds.

A new place that turns a component copy into an independent world must call `DetachVoxelVolume()`.
Without it, a Play-mode carve edits the edit scene. Entity copy/paste needs nothing: it goes through
YAML, which re-creates the volume.

Why not deep-copy in the copy constructor: the inspector's undo stores two component copies per
property edit and keeps a third as its snapshot, so a multi-megabyte volume would be cloned per edit.
Worse, each undo would swap in a new object, and every earlier `VoxelEditCommand` holds the old one,
so its undo would edit a volume no entity owns (#1561).

## 2. Seeded vs authored

`SeedFromHeightmap` marks a volume auto-seeded: it is a copy of the height field and can be rebuilt.
The first `SetVoxel` / `CarveSphere` / `AddSphere` makes it authored, and a brush stroke records the
previous state so its undo restores it. Scene files and save games write only authored volumes
(`TerrainComponent::AuthoredVoxelVolume`). A seeded greedy volume re-seeds on load, so persisting it
would add megabytes of float SDF data that compress poorly and that the tick would regenerate anyway.

The trap this guards against: a lost greedy volume re-seeds plausible geometry. Every persistence or
copy test must compare exact VOX1 bytes (`SerializeRLE`, which writes chunks in coordinate order)
on an **edited** greedy volume, never a mesh or chunk count.

## 3. Meshes follow the component, chunk dirty flags follow the volume

A component that receives a volume it has no meshes for sets `m_VoxelRemeshAll`, and the next tick
dirties every chunk. The volume's `Dirty` flags were already consumed by whoever meshed it first, so
without this flag the copy draws nothing (`VoxelMarchingCubesVisualEvidenceTest.UndoRestored…`, with a
negative control). Assigning the *same* volume keeps the meshes. A mesher switch re-meshes in the tick,
whatever made the switch (inspector, undo, MCP, script).

## 4. Persisted form

Scene YAML: `VoxelVolume: { VoxelSize, Size, Data: !!binary }`, the zlib-deflated VOX1 blob plus the
voxel size the volume was built with. That voxel size can differ from the component's `VoxelSize`
field, which the inspector edits without rebuilding the volume. The save game stores the same fields
as raw bytes. Asset packs store scene YAML text, so packs and the runtime need nothing more. Every size
is untrusted on read; see `VoxelOverride::DecodePersisted`.
