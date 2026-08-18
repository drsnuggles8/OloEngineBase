# A disk cache must not store a reference its reader cannot resolve

Issue #791. The `.omesh` mesh cache recorded a texture reference that pointed at nothing, so the
**second** load of a model came back with fewer textures than the first. Nothing errored. The first
run of the day always passed, which is why it read as a flaky test rather than as a broken cache.

Read this before adding a field to any persisted format that **names** something rather than
containing it — an asset handle, a file path, a URI, an index into another file.

## What stayed green

Everything. `AllVariants/DeccerCubesLoaderFixture.LoadsWithMeshesAndFiniteBounds` passed 7/7 on
every CI run and on every clean checkout, because a runner starts with no
`OloEditor/assets/cache/mesh/` and therefore only ever exercises the **cold** path. The failure
needed two loads in one working tree:

```
run 1 (cold cache)  -> [  PASSED  ] 7 tests.
run 2 (warm cache)  -> [  PASSED  ] 5 tests.  [  FAILED  ] 2  (TexturedComplex, TexturedEmbedded)
```

**Run 2 is a different experiment than run 1, not a repeat of it.** A test that only ever runs once
per checkout cannot see any cache bug, and every cache in this repo has that blind spot by
construction.

## The mechanism

`ImportedMaterialCodec` persists a material's textures **by reference** — an `AssetHandle`, with the
source path as a fallback — never by pixels. That is the right design (a texture shared by 40
materials costs 40 handles, not 40 copies). It has one precondition: every texture must *have* one
of those identities.

A texture embedded in a glTF/GLB has neither. It is bytes inside the model file: no file of its own,
no registry entry. So `Model::CookEmbeddedTexture` writes it out as a real `.png` and imports it,
after which it is an ordinary texture with a path and a handle, and the codec works unchanged.

The cook required an active project with an `EditorAssetManager`. Without one it returned early, the
texture stayed an in-memory decode with no path and no handle, and then:

1. `Describe()` produced an **empty** `TextureRef` for the slot — which is also, exactly, what it
   produces for a slot that was legitimately never set;
2. `Encode()` wrote that empty reference into the `.omesh`, reporting success;
3. `Realize()` on the next load resolved it to nothing and left the slot unset — which is also,
   exactly, the correct handling of a slot that was legitimately never set.

Every step behaved correctly in isolation. The information that the slot *had* held a texture was
destroyed at step 1, so no later step could notice.

## The three rules

**1. A cook that gives a thing its identity must not be conditional on something unrelated.**
The cook needed a *directory to write to*; it was gated on *an asset registry to register with*.
Those are separate capabilities and only the first is required for the cache to work — the reader
resolves the recorded path before it ever consults the handle. Splitting them was the fix: the
directory comes from `MeshCache::GetEmbeddedTextureCacheDirectory()` (which falls back to a
CWD-relative `assets/` exactly as the `.omesh` itself does, so the texture always lands under the
same root as the file referencing it), and registration is best-effort on top.

Note the asymmetry that hid this: with a project mounted the whole path worked, and
`ImportedMaterialPackTest.EmbeddedTextureSurvivesTheOmeshCache` proved it. The test mounted a
project because that was the realistic editor case. The *un*realistic case was the one every
headless load actually took.

**2. Refuse to persist what you cannot resolve.** `Describe()` cannot fail — an unrepresentable
texture degrades to an empty slot rather than an error, which is correct for a codec. So the
**writer** must ask the question instead. `ImportedMaterialCodec::CanPersistEveryTexture()` reports
whether every slot that is *set* describes to something resolvable, and `Model::LoadModel` omits the
material table entirely when it does not — sending the next load down the re-import branch.

That trade is not close. A cache that is slow is a cost; a cache that is wrong is a bug, and one
that is wrong *only on the second run* is a bug that will be diagnosed as flakiness. Degrade to slow.

**3. A writer fix does not fix the files already on disk.** Every machine that had run the suite
held `.omesh` files containing the empty references, written by the old code and still perfectly
valid by the header check. Fixing only the writer produces the worst possible symptom — "works on a
clean machine, still broken on mine" — which is precisely the shape that made this hard to see in
the first place. `OMeshFormat::CurrentVersion` was bumped 4 → 5 with no new sections, purely to
invalidate them, exactly as v3 had been bumped before it. `ReadTimestamp` gates cache validity on
`Version == CurrentVersion` (strict), so the bump costs one cold re-import and nothing else.

## Where else this shape lives

The same defect was sitting one function away, unreported: `Model` packs the legacy/specular
workflow's separate metallic and roughness files into one texture **in memory**, so that texture had
no path either and every warm load of `backpack.obj` silently lost its metallic-roughness map. It is
now cooked to a file through the same helper. Grep for a `Texture2D` built from a
`TextureSpecification` rather than a path and ask what persists it.

More generally, look for anything that is **synthesised at import time and then referenced by name**:
a runtime-generated atlas, a procedurally-built LUT, a merged or remapped buffer. If a consumer
persists it by identity, the importer owes it an identity — a real file — not just pixels on the GPU.

Distinct from the staleness species (a cache keyed on path + config that never notices the source
changed — issues #505, #530, #542). There the cached artifact is *out of date*; here it was fully
up to date and internally consistent, and a field inside it pointed at nothing.

## The test that would have caught it

`DeccerCubesLoaderFixture.SurvivesAWarmMeshCacheRoundTrip` — invalidate, load cold, assert the
`.omesh` was written, load warm, and compare. It compares **pixels read back off the GPU**, not just
"is there an albedo map": a material holding a handle to the *wrong* texture satisfies the weaker
assertion.

Two things to copy when writing one for another cache:

- **Assert the cache was actually written** between the halves (`MeshCache::IsMeshCacheValid`).
  Without that, a warm half that silently re-ran the cold path passes while proving nothing — the
  most likely way this kind of test rots.
- **Take the `RESOURCE_LOCK`.** The mesh cache is a real repo-relative directory, not a
  process-isolated temp dir, so a test that invalidates it races every other test loading the same
  model under `ctest --parallel`. Add the case to `OLO_MESH_CACHE_TESTS` in
  `OloEngine/tests/CMakeLists.txt`.
