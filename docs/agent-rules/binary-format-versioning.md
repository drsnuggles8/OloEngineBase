# One current version per on-disk format

**A reader accepts exactly its format's current version. Any other version is rejected with a
message that says how to fix the file: regenerate it (a cache or a cooked artefact) or re-save it
(a scene or a save game). Bumping a format means migrating the repo's own content in the same PR.**

No range checks, no per-field version gates, no read-and-discard fields, no fallback that guesses an
old layout, and no empty migration scaffold kept "for the next change". Git history is where the
old layouts live.

## Why

The engine has no external users, and no binary content is checked in: there is no `.olosave`,
`.olopack` or `.omesh` in the repo. So a reader for an older version protects only a stale file on
one developer's disk, which the fix message tells them how to replace. Caches regenerate; scenes are
text and are migrated in the PR that changes them.

The previous version of this guide *required* compat: a range-accepting header check, a
`HasFieldsSince` gate on every new field and a no-op migration chain. Agents followed it, so every
bump added a branch that never went away. By #1496 the save game carried 38 versions and 47 gates,
the asset pack had an empty `MigrateAssetPackIndex`, and six other formats kept a reader for a
layout nothing could still produce. Parallel branches also collided on the version number, and the
loser had to renumber a gate chain after the merge.

## What to do when a format changes

1. **Change the writer and the reader in place**, to the new layout.
2. **Bump the version constant once per PR**, not once per commit.
3. **Reject everything else.** The check reads, in substance:

   ```cpp
   if (header.Version != CurrentVersion)
   {
       OLO_CORE_ERROR("{}: format v{} is not supported (this build reads v{} only). Re-save it / "
                      "regenerate it from the source asset.", path, header.Version, CurrentVersion);
       return false;
   }
   ```

   A version *above* current is rejected by the same check. Name the fix in the message; "invalid
   version" alone leaves the reader to work out whether the file or the build is wrong.
4. **Migrate the content in the same PR.** Text formats (scene `.olo`, asset YAML, input actions)
   are rewritten by a script and then checked by loading every file, not by counting edits. Binary
   caches need nothing: they are rejected and regenerated.
5. **Replace the tests that built an old-version file** with one wrong-version rejection test. A
   fixture that hand-synthesises an older archive is compat code in the test tree.

If two branches both bump to N+1, the second to merge keeps N+1 or moves to N+2; there is no chain
to renumber.

`Scene/SceneBinaryFormat.h` has always worked this way (`MinSupportedVersion` moves with
`CurrentVersion`; its comment explains why for a cache) and is the reference.

## A fixed-order archive must be framed and length-checked

An `FArchive`-backed format is not self-describing: each `ar << field` reads the next N bytes in the
order they were written. If the reader and the writer disagree about one field, every later read
desyncs and reinterprets bytes as the wrong field, with no error. The single-version rule removes
the *old-layout* source of that desync; it does not remove truncation or a bug.

So a fixed-order archive is written as **length-prefixed blocks**, and the reader checks, once per
block, that it consumed exactly the declared length. A short or long read is a loud failure naming
the block. Do not probe `ar.AtEnd()` in the middle of a block to decide whether a field is present:
that probe cannot tell "older archive" from "truncated archive", and under this policy there is no
older archive.

## Format inventory

| Format | Version constant | On mismatch |
|---|---|---|
| Save game (`.olosave`) | `kSaveGameFormatVersion` (`SaveGame/SaveGameTypes.h`) | rejected: re-save (`SaveLoadResult::UnsupportedVersion`) |
| Asset pack (`.olopack`) | `AssetPackFile::Version` (`Serialization/AssetPackFile.h`) | rejected: rebuild the pack |
| Mesh cache (`.omesh`) | `OMeshFormat::CurrentVersion` (`Serialization/MeshBinaryFormat.h`) | rejected: re-imported from the source |
| Animation cache | `AnimationBinarySerializer` header version | rejected: re-imported from the source |
| Scene sidecar (`.scenebin`) | `OSceneFormat::CurrentVersion` (`Scene/SceneBinaryFormat.h`) | rejected: re-read from the `.olo` |
| Scene YAML (`.olo`) | `SceneSerializer::CurrentVersion`, the required `Version:` key | rejected: migrate the file in the PR |
| Imported-material codec | `ImportedMaterialCodec::CurrentVersion` | rejected: re-imported |
| Virtual-geometry cook (OVGS) | `kSetVersion` (`Renderer/VirtualGeometry/VirtualMesh.cpp`) | rejected: re-cooked |
| Asset registry (`.oar`) | `AssetRegistry::FileVersion` (`Asset/AssetRegistry.h`) | rejected: moved to `AssetRegistry.oar.rejected`, then the editor's asset scan writes a fresh one with new handles; restore the original from git to keep scene references |
| Sound-graph compiler cache | `CompilerCache::FormatVersion` (`Audio/SoundGraph/CompilerCache.h`) | rejected: cache miss, recompiled |
| Mesh collider cache (`.omc`) | `OloMeshColliderHeader::CurrentVersion` (`Physics3D/MeshCookingFactory.h`); it also versions the headerless Jolt shape blobs inside it | rejected: re-cooked |
| Voxel override RLE (`VOX1`) | `VoxelOverride::RLEVersion` (`Terrain/Voxel/VoxelOverride.h`) | rejected: re-save the voxel edits |
| Lightmap, volume, groom, groom binding | `*BinaryFormat::CurrentVersion` | rejected: re-baked / re-imported |
| Texture import sidecar (`.oloimport`) | `kSidecarVersion` (`Renderer/TextureImportSettings.cpp`), the required `Version:` key | rejected with an error naming the sidecar; the texture still cooks with automatic settings until the sidecar is fixed |
| Benchmark capture manifest (YAML) | `Benchmark::kCurrentManifestVersion` (`Renderer/Benchmark/BenchmarkManifest.h`) | rejected: migrate the manifest in the PR (copies under `docs/testing/evidence/` are historical and not read) |

When you add a format, add a row.

## Guard

Each format has a wrong-version test that writes a current file, patches its version field, and
asserts the load fails with the fix message. `SaveGameFileTest.cpp`, `AssetPackTest.cpp` and
`MeshBinarySerializerTest.cpp` hold the pattern.
