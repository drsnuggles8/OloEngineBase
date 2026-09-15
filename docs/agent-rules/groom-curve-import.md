# Groom curve import, cooking and preview (#1232)

Read before touching `OloEngine/src/OloEngine/Groom/`,
`Asset/Interchange/Alembic/AlembicGroomImporter.*`, `Asset/Serializers/GroomSerializer.cpp` or
`Serialization/GroomBinaryFormat.h`.

## The rules

1. **Polygon Alembic import is not curve support.** `AlembicMeshImporter` reads `IPolyMesh` /
   `ISubD`; a groom is `ICurves` and goes through `AlembicGroomImporter`. A groom exported as
   ribbons is a mesh, and importing it as one loses per-strand identity, root UVs and the guide
   flag.

   **Where that routing actually happens, and why it is not in `AssetExtensions`.** `.abc` maps to
   `AssetType::MeshSource`, so the generic import path always sends it to `AlembicMeshImporter` —
   one extension cannot express two schemas. The groom route is the content browser's
   **"Import as Groom"** action (`ContentBrowserAction::ImportGroom`), which calls
   `ArchiveContainsCurves`, then `AlembicGroomImporter::Import`, then `GroomCooker::CookToBytes`,
   and registers the sibling `.ologroom` it writes. That is deliberately the same shape as the
   `.vdb` → `.olovol` cook next to it: a user-initiated action, because deciding by content means
   reading the file, and `EditorAssetManager::ImportAsset` is metadata-only by design (the watcher
   calls it on files still being flushed).

   This was missed the first time: the predicate and the importer existed, were tested, and were
   called by **nothing** — so a groom `.abc` could not be imported in the editor at all, and the
   only `.ologroom` files in the repo had been written by a test. If you add a second source format,
   give it an action; do not widen the extension map.

2. **Reject, never clamp.** Malformed curve data and unknown `groom_*` attributes fail the import
   with a diagnostic that names the attribute or the invariant. A clamp is the silent fallback this
   repo forbids: a groom that imports minus its authored intent is undetectable downstream.
   Non-`groom_` arbGeomParams are DCC bookkeeping and are ignored (logged at TRACE), because
   refusing those would make ordinary exports un-importable.

3. **The cook is a canonicalisation, not a transform.** It stable-sorts curves so each group is one
   contiguous range, recomputes the derived data, and writes. It does **not** tessellate, resample
   or remap widths — the sample count a strand needs is a rendering decision (#1246), and baking one
   at import time fixes it forever at the wrong end of the pipeline. That is also what makes the
   round-trip test meaningful rather than a test of the cook's own rounding.

4. **Determinism is a tested contract.** Same source ⇒ byte-identical `.ologroom`, across runs and
   machines. The leaks to keep closed: unordered iteration (group ids come from first-appearance
   order, never from hash-map order), an unstable sort (`std::stable_sort`, and
   `GroomCookDeterminismTest.CookIsIndependentOfTheSourceCurveOrder` is the case that catches
   `std::sort`), uninitialised padding (every wire struct is explicitly padded and size-asserted),
   timestamps and absolute paths in provenance (there are none), and text-formatted floats (there
   are none — IEEE-754 bit patterns end to end).

5. **Widths are diameters, in object-space units.** The Alembic/USD `widths` convention. A source
   transform scales them by the mean axis length; a non-uniform transform is accepted but logged,
   because one scalar cannot describe an anisotropically scaled strand.

6. **Bounds include the strand radius.** `RecomputeDerivedData` widens the point AABB by the largest
   radius. A groom culled on its centreline pops at the screen edge. An *empty* groom gets a zero
   box, not the inverted-infinity sentinel a naive min/max seed leaves behind — that value reads as
   "visible from everywhere" to a culling test.

7. **A declared group with no curves is an error, not a cleanup.** Dropping it would renumber every
   later group id, so a groom's group indices would change meaning between two cooks.

8. **Reading a `.ologroom` needs no Alembic.** `GroomSerializer` is pure binary decode; only the
   importer is `OLO_WITH_ALEMBIC`-gated. Same cook/runtime split `.olovol` uses for OpenVDB.

## The documented curve input convention

The authoritative copy is the header comment in
[`AlembicGroomImporter.h`](../../OloEngine/src/OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h)
— it is what an importer change must keep true. In summary, per `ICurves` prim at sample 0:

| Source | Meaning | Rejected when |
|---|---|---|
| `P` | control points, **root first** | non-finite, or past `GroomLimits::MaxCoordinate` |
| `nVertices` | per-curve point counts | they do not sum to `P`'s size; a count below 2 |
| `type` | `kLinear` → `Linear`, `kCubic` → `BSpline` | `kVariableOrder`; two prims disagreeing |
| `wrap` | open strands only | `kPeriodic` — a hair strand is not a loop |
| `widths` | diameters | scope other than constant/uniform/varying/vertex; wrong value count; negative |
| `uvs` | the **root** UV | scope as above. Vertex scope keeps the root's value and logs the discard |
| `groom_guide` | uniform int32, non-zero = guide | not uniform int32; wrong value count |
| `groom_group` | uniform int32 sub-group → `<prim path>#<n>` | as above |
| any other `groom_*` | — | always: unimplemented groom semantics |

Absent `widths` substitutes `AlembicGroomImporter::kDefaultWidth` and **says so**; absent `uvs`
gives `(0,0)` root UVs and says so. Both are announced substitutions, not silent ones.

## The preview is the acceptance surface

`GroomPreview` draws strands, roots, per-group colour and a root→tip brightness ramp. The ramp is
the only direction cue in the picture: a groom imported tip-first still looks like hair without it.
The preview **subsamples with a stride** rather than truncating — a cap that drew the first N
strands would show one group of a three-group groom and look perfectly fine doing it — and
`GuidesOnly` strides over the guide subset, not the whole groom.

Two caps set that stride, and the tighter one wins (`PlanGroomPreview`, which is a pure function
precisely so the rule is testable without a GL context):

* `MaxStrands` — the authored intent.
* `MaxSegments` — **the one that actually bites.** Every debug line takes one entry of the frame's
  shared 65536-transform buffer, and a strand costs one line per segment *plus three* for its root
  cross. Two reference grooms at `MaxStrands = 2000/2500` submitted ~39,500 lines and the editor
  logged `FrameDataBuffer: Transform buffer overflow!` every frame. The default is 8000 and is
  deliberately well under the raw headroom: it is PER GROOM, a scene may hold several, and the
  shadow/depth passes re-submit. `GroomPreviewStats::SegmentBudgetLimited` says when this is the
  binding cap, so "I raised Max Strands and nothing changed" has an answer, and
  `SegmentBudgetExhausted` says when the exact cap stopped submission mid-groom.

`Renderer3D::DrawLine` **builds a packet and returns it — it does not queue it.** Pair it with
`Renderer3D::SubmitPacket` (see `DrawWorldAxisHelper`). Forgetting is completely silent: the draw
code runs, the counters advance, the screen is unchanged. Its `thickness` is likewise a WORLD-space
width in units of 5 mm, not pixels, so content at real scale needs a thickness derived from its own
bounds.

Keep it debug geometry. Hair shading is #1246/#1247; drifting into it here removes the plain picture
that makes a bad import obvious.

## Where the touch-points are

A groom is a new asset type, so the CLAUDE.md *Definition of done* checklist applies in full:
`AssetType::Groom`, `AssetExtensions` (`.ologroom`), `AssetImporter`'s serializer registry,
`Asset.h`'s serializer friend list, `GroomComponent` in `Components.h`, the save-game
`Serialize` + `REGISTER_SAVE_COMPONENT` pair (**not** generated), and the editor inspector +
Add Component entry (**not** generated). Scene YAML and the `AllComponents` tuple are generated by
OloHeaderTool because `GroomComponent` is all-trivial and public.
