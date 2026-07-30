# A missing optional asset must degrade — and a constructor assert makes that impossible

From issue #694: `VirtualGeometryStress.olo` aborted on load with `Submesh index out of
range!` on any clone that had not run `scripts\Fetch-Assets.ps1`. **The default state of a
fresh clone was the crashing state**, and two written contracts (the fetcher's own
`.DESCRIPTION` and the scene file's header) promised behaviour the code did not deliver.

## 1. A precondition asserted in a constructor delegates safety to every call site — and they will disagree

`Mesh`'s constructor asserted both of its preconditions:

```cpp
OLO_CORE_ASSERT(m_MeshSource, "MeshSource is null!");
OLO_CORE_ASSERT(m_SubmeshIndex < static_cast<u32>(m_MeshSource->GetSubmeshes().Num()), "Submesh index out of range!");
```

There were ~30 `Ref<Mesh>::Create` sites. Most guarded. Some did not, and they were the ones
that mattered:

| site | guard | consequence |
|---|---|---|
| `MeshSerializer::TryLoadData` | clamps + warns, with a comment naming the hazard | fine |
| `MeshSerializer::DeserializeFromAssetPack` | **none** — null source *and* an unvalidated index read off a binary stream | the **shipped-runtime** path; a truncated pack aborts `OloRuntime` |
| `PlaceholderMesh::CreatePlaceholderMesh` | **none** | the actual firing frame — see §2 |

The lesson generalises past `Mesh`: **when a type's inputs come from data the process does
not control — a deserialized field, an asset handle that may not resolve, a byte range off
a file — its constructor cannot be the thing that enforces the invariant.** An assert there
does not prevent the bad state; it relocates responsibility to N call sites and guarantees
they drift. Model the invalid state instead and make it observable (`IsValid()`), which
`Mesh` already did — `Mesh() = default` produced exactly the state the ctor refused to build.

Note the second assert failed as `0 < 0` for a source with **no** submeshes, so even the
perfectly ordinary `submeshIndex = 0` aborted. A bounds check whose lower bound can equal
its upper bound rejects the common case, not just the exotic one.

## 2. The recovery path for a missing asset was itself the crash

The firing frame was not either serializer. It was the **placeholder machinery added to
survive missing assets** (#455):

```text
Scene virtual-mesh loop
  -> AssetManager::GetAsset<MeshSource>(<un-fetched dragon>)
  -> ResolveAssetOrPlaceholder            (handle registered, file absent)
  -> PlaceholderAssetManager::GetPlaceholderAsset(AssetType::MeshSource)
  -> PlaceholderMesh::CreatePlaceholderMesh
  -> Ref<Mesh>::Create(meshSource)        <-- MeshSource(vertices, indices) adds NO submesh
  -> assert 0 < 0
```

`MeshSource`'s `(vertices, indices)` constructors never call `AddSubmesh`, so the stand-in
cube had no drawable range at all — it was not `IsValid()`, `GetIndexCount()` was 0, and it
could never have rendered even had the assert passed. **The fallback had never once been
exercised**, because no test asked for a `Mesh`/`StaticMesh`/`MeshSource` placeholder (the
one placeholder test used `AssetType::Terrain`, which maps to `GenericPlaceholder`). 5170
green tests said nothing about it.

When you add a degradation path, write a test that *takes* it. A fallback with no coverage
is not a fallback; it is untested code on the unhappiest path you have.

## 3. "Fail soft" is not done until every accessor is total

Relaxing the ctor immediately surfaced a second abort one call deeper —
`MeshSource::GetVertexArray()` asserted `"VertexArray not initialized. Call Build() first."`
That assert fired on the very null-check written to avoid it: the submission paths spell
"nothing to draw" as `if (!mesh->GetVertexArray())` (`Renderer3D::DrawMesh` does exactly
that and logs an error). An assert on a plain getter that callers already null-check is the
same anti-pattern as the ctor's.

So relaxing a constructor is only half the change. Every accessor reachable on the now-
representable invalid object has to be total, or you have moved the crash rather than fixed
it. For `Mesh` that meant `GetVertices`/`GetIndices` returning a shared empty container,
`GetVertexArray` returning null, and `GetSubmesh` returning an empty `Submesh` — an empty
range every draw path already treats as "nothing to draw". Keep the asserts on the
accessors nothing null-checks (`GetVertexBuffer`/`GetIndexBuffer`): reaching one of those
unbuilt really is a call-order bug.

**Do not silently substitute a neighbouring value.** `Mesh`'s ctor deliberately does not
clamp an out-of-range index to 0 the way `SetMeshSource` does: drawing a different submesh
is a wrong picture, drawing nothing is an honest one. A caller that genuinely wants the
clamp does it at its own seam with its own message. (Same reasoning as `OLO_SERIALIZE(Reject)`
vs `Clamp` in the serializer — see CLAUDE.md.)

## 4. "Degrades gracefully" means ONE message, not a flood

After the aborts were gone the scene loaded — and printed
`Cannot load asset: file does not exist: Assets/Models/Stanford/xyzrgb_dragon.ply` **every
frame, for each of 24 entities**, burying everything else in the console. Nothing caches the
substituted placeholder in `EditorAssetManager` (deliberately — so fetching the file
mid-session is picked up), so every frame re-attempts and re-logs.

A per-frame resolution failure needs a warned-once set. The engine now has three instances
of that idiom (`AssetManager::ResolveAssetOrPlaceholder`, `Scene`'s virtual-mesh loop,
`EditorAssetManager::LoadAssetFromFile`) — reuse it rather than inventing a fourth shape.
The message also names `scripts\Fetch-Assets.ps1`, which is what the fetcher's contract
asks consumers to do.

## 5. Reproducing this class of bug: the trigger is *resolution*, not *loading*

The scene loads perfectly on the **Forward** path, because `Scene`'s virtual-mesh loop is
gated on Deferred (`Scene.cpp`, "Gated on Deferred, and the gate must live HERE"). Nothing
resolves the handle, so nothing substitutes a placeholder, so nothing crashes. The editor
also starts on Forward. So:

- **A "load the scene and see if it crashes" check can pass while the bug is fully present.**
  What triggers it is the first thing that *resolves* the handle.
- `AssetSceneLoadTest` could not catch it either: its carve-out `continue`d past a
  registered-but-absent asset *before* resolving it. Rewriting the carve-out to resolve the
  handle and only then skip the "did it load" check turns that line into the assertion.
- The renderer path is not scene-serialized and not persisted, so reaching the Deferred-gated
  code in the live editor needs the UI. For a one-shot verification, temporarily defaulting
  `RendererSettings::Path` to `Deferred`, building, and reverting is cheaper than driving
  the ImGui combo — but it is a ~20-minute rebuild each way, since `RenderingPath.h` is
  widely included.

Prefer a headless test that calls the resolving API directly
(`AssetSceneLoad.MissingOptionalAssetDegradesInsteadOfAsserting`). It reproduces the abort
in 1 ms, needs no GL context, and — unlike the scene loop — still runs on a clone that *has*
fetched the optional asset.
