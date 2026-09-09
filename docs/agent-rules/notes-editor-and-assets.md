# Subsystem notes — editor panels, asset pipeline, platform utils

Accumulated gotchas from work on the Content Browser, `EditorAssetManager`, the texture cook, and
the cross-platform file dialogs. Reference notes, not failure postmortems — see
[README.md](README.md) for the postmortem docs.

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. The Content Browser reads the filesystem, not the asset registry

The grid is built from a cached filesystem tree (`Panels/ContentBrowser/DirectoryTree.h`).
**Importing a file into `EditorAssetManager`'s registry does not make it appear** — the panel
re-scans a directory only when its `DirectoryInfo::NeedsRefresh` is set.

To surface a file live without the user pressing F5, call
`DirectoryTree::MarkDirty(<asset-root-relative dir>)` (marks the dir and its ancestors);
`ContentBrowserPanel::RefreshIfDirty()` at the top of `OnImGuiRender` turns it into a
`SafeRefreshSubtree` + `RefreshVisibleItems` on the next paint.

**Path-base trap.** The watcher and registry use **project-relative** paths
(`Project::GetProjectDirectory()`), but the DirectoryTree root is `Project::GetAssetDirectory()`
(`<project>/Assets`). Convert with
`std::filesystem::relative(absPath.parent_path(), assetRoot)` and reject a leading `..`.
`AssetImportedEvent` carries an **absolute** path precisely so listeners don't need the base.

## 2. The filewatch auto-import seam

`EditorAssetManager::OnFileSystemEvent` (the `filewatch::FileWatch` callback, under
`OLO_ASYNC_ASSETS`):

- **Runs on the game thread** — the callback wraps the handler in `Tasks::EnqueueGameThreadTask`.
  Firing engine events (`Application::Get().OnEvent`) inline from it is safe.
- **A new file arrives as `filewatch::Event::added`**, and a write-temp-then-rename save as
  `renamed_new` — *not* `modified`. The handler historically acted only on `modified`, which is
  exactly why new files were never auto-imported. The presence set is
  `{added, modified, renamed_new}`; `{removed, renamed_old}` are drops.
- **`AssetRegistry` is internally mutex-locked** (`AddAsset`, `GetHandleFromPath`,
  `GenerateHandle` are each thread-safe), so `ImportAsset`'s registry calls don't need the
  manager's `m_RegistryMutex`. `ImportAsset` is idempotent and registers **metadata only** — safe
  to call on a file still being flushed to disk.
- **Hot-reload only assets that are currently loaded** (`IsAssetLoaded`). Otherwise a large new
  file spams partial-content reloads as it streams in. The decision lives in the pure,
  unit-tested `DecideFileWatchAction` (`Asset/AssetFileWatchPolicy.h`).
- **"Currently loaded" is decided by whether anything actually *resolved* the handle this
  session, which a renderer setting can gate — so `Reload` can look permanently dead when it is
  merely idle.** `m_LoadedAssets` is populated by `EditorAssetManager::GetAsset`, i.e. by a
  consumer asking for the asset. A `VirtualMeshComponent`'s mesh source is resolved *only* by the
  submission loop in `Scene.cpp`, which runs **only on the Deferred path** — so with the editor on
  Forward, opening `VirtualGeometryTest.olo` loads no mesh at all, every filewatch event on those
  files reports `loaded=false`, and every decision is `Ignore`. Nothing is wrong; nothing asked.
  This cost an hour during #863 and produced a nearly-filed phantom bug ("hot-reload is unreachable
  for scene assets"). **Before concluding a path is unreachable, check `olo_virtual_geometry_stats`
  — its `renderingPath` field and its own `note` say this in plain words — and confirm a
  `Loaded asset: <path>` trace exists for the asset you are about to touch.** An absence in the log
  is not evidence of a broken code path; it is equally evidence of an idle one. Related:
  [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md) §6, on why
  this seam had no headless coverage at all.
- The registry file is `<project>/AssetRegistry.oar`; `.oar` maps to no `AssetType`, so persisting
  it cannot re-enter the handler. It is **git-tracked** — revert it after any live drop-file test.

## 3. Missing-asset placeholders: intercept at the typed facade

The right interception point is `AssetManager::GetAsset<T>()`, **not** per-field deserialize in
`SceneSerializer.cpp`.

The static type `T` is only known at the facade. The virtual `RuntimeAssetManager::GetAsset(handle)`
can substitute only when pack metadata gives it the type — i.e. a *valid-but-unresolvable* handle.
A *totally absent* handle (a scene referencing a deleted asset) has unknown type, so only the
facade can substitute via `T::GetStaticType()`. The fix is therefore layered: the managers
substitute when they know the type, and the facade substitutes by `T` for fully-dangling handles.

**Placeholders WRAP the real asset.** `PlaceholderTexture` *holds* a `Ref<Texture2D>`; it is not
one. So `asset.As<Texture2D>()` on a placeholder returns null, and the facade must unwrap
(`UnwrapPlaceholder`). Before this, the editor's pre-existing missing-file placeholder was
effectively useless — the cast failed and the caller got null anyway.

Warn **once per handle** (static set + mutex at the facade, `m_WarnedUnresolvableHandles` in the
runtime manager) or a dangling ref touched every frame floods the log.

> **Two traps this uncovered.** Adding `T::GetStaticType()` to `GetAsset<T>` broke compilation for
> asset types whose `GetStaticType()` was accidentally `private` (`EnvironmentMap`, `Model`) — the
> Asset interface is conventionally public on every asset type. And **MSVC passed while all three
> Linux Clang jobs failed to compile**: `.As<T>()` called directly on an expression containing
> `T::GetStaticType()` is type-dependent, so Clang correctly requires `.template As<T>()`. Bind to
> a concrete `Ref<Asset>` local first. **Always `cmake --preset clangcl` before pushing a header
> you touched templates in** — MSVC silently accepts two-phase-lookup violations Clang rejects.

## 4. Grep before naming a new top-level `OloEngine::Xxx`

The `AssetType::SoundConfig` asset is implemented by class **`SoundConfigAsset`**, not
`SoundConfig` — `OloEngine::SoundConfig` was already taken by an unrelated runtime playback struct
in `Audio/SoundGraph/SoundGraphSound.h`, which `Scene/Components.h` drags into nearly every TU. A
second top-level `SoundConfig` is a hard ODR redefinition.

Only the asset *class* takes the suffix; the enum value, the `.olosoundc` extension, the YAML root
key and the serializer keep the plain name. This matches `MeshColliderAsset` / `MaterialAsset`.

**`Components.h` has a very large transitive include set, so a name clash surfaces as a
redefinition error far from where you added the type.** Prefer the `XxxAsset` suffix.

## 5. Runtime-only per-`Scene` state — don't reach for a component

To attach runtime-only state to a `Scene` (a focus target, event delegates, a cache), mirror the
`GameplayEventBus` / `UINavigation` pattern:

1. New class in its subsystem header.
2. `Scene.h` — forward-declare it, add `std::unique_ptr<T> m_X;`, declare `T& GetX();` plus a const
   overload (define out-of-line so the header only needs the forward declaration).
3. `Scene.cpp` — include the full header, init in the ctor **in declaration order**, define the
   accessors, and `m_X->Clear()` in `OnRuntimeStop`.

A new `struct *Component` would instead be swept by OloHeaderTool into the generated tuple /
SaveGame / `OnComponentAdded` / serializer `.inl` and force edits or exclusion entries across
several files. Per-`Scene` `unique_ptr` state touches none of them.

> Gotcha: a helper that reads the state class's private members must be a **member of the friend
> class**. An anonymous-namespace free function in the same TU is *not* covered by a
> `friend class X;` grant (C2248).

## 6. `bc7enc_rdo`: the partial params initializers encode from garbage

`bc7enc_compress_block_params_init_linear_weights(&p)` and `..._init_perceptual_weights(&p)` set
**only** `m_perceptual` and `m_weights[4]`. They leave `m_mode_mask`, `m_max_partitions`,
`m_uber_level` and the error-weight floats uninitialized. Calling one on a fresh stack struct
encodes from garbage — bad blocks, low PSNR.

Correct order: `bc7enc_compress_block_params_init(&p)` first (sets every field, defaults to
perceptual), *then* `..._init_linear_weights(&p)` only if you want linear weighting.

Also: `bc7enc_compress_block_init()` and `rgbcx::init()` must each be called once before encoding
(guard with `std::call_once`); compile only `bc7enc.cpp` + `bc7decomp.cpp` + `rgbcx.cpp`.

## 7. BC6H is a hand-written mode-11 encoder validated against an independent decoder

The vendored `bc7enc_rdo` ships **no BC6H encoder**, so `EncodeBC6HBlockUnsigned` in
`Renderer/TextureCompression.cpp` is ours. It emits only **mode 11** (1 subset, two raw 10-bit
endpoints, 4-bit indices), unsigned only — IBL radiance is non-negative.

Validation decodes with vendored **`bcdec`**, deliberately an *independent oracle*: a matched
encoder/decoder bug cannot hide.

Exact mode-11 layout (LSB-first): mode `0b00011` (5 bits) → `rw,gw,bw,rx,gx,bx` (10 bits each) →
`index[0]` (3 bits, anchor MSB implicit-0) → `index[1..15]` (4 bits each), texels row-major.

Decode math the encoder must invert: unquantize
`(e==0)?0:(e==1023)?0xFFFF:((e<<16)+0x8000)>>10`; interpolate `(a*(64-w)+b*w+32)>>6` with
`aWeight4 = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64}`. **Index 13 is 55, not 56** — the table
is asymmetric, so an anchor fixup must *re-select* indices after an endpoint swap, never just
invert them.

GL upload maps `ImageFormat::BC6H` → `GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT`; the no-BPTC fallback
decodes to **RGBA16F**, not RGBA8, to preserve HDR. Pack auto-cook picks BC7 (LDR) / BC6H
(`stbi_is_hdr`); **BC5-for-normals is deliberately not auto-selected** — a filename heuristic can't
tell a 2-channel normal from other linear data. A cook failure falls back to the raw record so a
build never fails.

## 8. `.olocine` versioning: tolerant reader, version for provenance only

The reader defaults every field (`.as<T>(default)`) and finite-checks floats, so adding a field is
back-compatible for free. **Bump `kCinematicSequenceVersion` for provenance only** — the reader
warns on a newer version and proceeds; it never branches on version to parse.

**Emit new per-key fields unconditionally.** A Bezier segment reads the *left* key's `OutTangent`
and the *right* key's `InTangent`, so gating emission on `key.Interp == Bezier` silently drops the
right key's tangent. Pinned by `CinematicSerializerTest.BezierTangentsRoundTrip`.

The cinematic value types are plain structs, not ECS components — no cross-binding dance.

## 9. Platform utils

- **`FileDialogs::ShowInFileManager(path)`** (`Utils/PlatformUtils.h`) is the canonical
  cross-platform reveal helper. Windows uses `ShellExecuteW` (`/select,"<file>"` for a file,
  `explore` for a dir); Linux double-forks `xdg-open` on the containing dir (xdg-open has no
  file-select mode). Do **not** add another Windows-only `ShellExecuteW`, and don't copy
  `ContentBrowserPanel::OpenInExplorer` — it's a Windows-only private member with a warn stub
  elsewhere. `ShellExecuteW` needs an explicit `#include <shellapi.h>`; `WIN32_LEAN_AND_MEAN`
  excludes it.
- **Linux file dialogs** shell out to `zenity` or `kdialog`, chosen by `$PATH` preferring the one
  matching `$XDG_CURRENT_DESKTOP`. No vendored dialog library exists (tracy's bundled `nfd` is
  tracy-internal). The `filter` argument is the **Win32 commdlg format** (double-NUL-terminated
  `desc\0glob;glob\0`) on both platforms — the Linux side parses and translates it; don't invent a
  second format. An empty return means *both* user-cancel and no-backend-available; the two are
  distinguished only in the log.
- Deliberate Linux non-parity, leave alone: ELF icon embedding and the crash-reporter per-arch
  register dump.

## 10. `Video/PlMpeg.cpp` is vendored, not first-party

It is the single TU that compiles the third-party pl_mpeg amalgam
(`#define PL_MPEG_IMPLEMENTATION`). Treat it like `OloEngine/vendor/`. Its `#include <stdio.h>` is
**required** (pl_mpeg's `FILE*` API must be visible before the implementation section), so
SonarQube `cpp:S988` "remove this include" is a false positive there.

The first-party wrapper is `Video/PlMpegBackend.cpp` and *is* fair game.

## Splitting one model into several entities (issue #899)

`Model` flattens the imported node graph under `aiProcess_PreTransformVertices`
and draws every submesh with the entity's single transform, so **there is no way
to move one part of a `ModelComponent` relative to another**. When a part of a
model has to animate independently — a sail that trims, a turret that traverses,
a door that opens — the cheap answer is not an engine feature, it is to split the
source file.

The glTF importer does keep node names (`ProcessMesh` writes
`submesh.m_NodeName`), so a `.glb` authored with named nodes can be subset into
one file per moving part. `scripts/split-glb-nodes.py` does it over the JSON
chunk: keep the nodes you want, keep every material/texture/image entry, and
repack only the `bufferView`s the surviving accessors reach. `ship-small.glb` went from 116 KB to
a 104 KB hull and a 12 KB sail this way, both still pointing at the one shared
external atlas, so the pair costs no extra texture memory.

**The load-bearing part is the RE-ORIGIN, and it is easy to miss.** Because the
node transforms are baked into the vertices, the extracted part's geometry is
still at its position inside the whole model. Rotating an entity that carries it
swings the part around the MODEL's origin, not around its own hinge. Dropping the
extracted node's `translation` puts its own pivot at the file origin, and the
scene then places the entity at exactly that translation in the parent's space —
so the split is invisible and a plain Y rotation on the child is the hinge angle.
Get it wrong and the part still moves, plausibly, around the wrong point.

Two more things that bite:

* **`.glb` is a supported asset extension**, so any new one under
  `SandboxProject/Assets/` must be in `AssetRegistry.oar` or
  `AssetContentValidity.EverySupportedAssetOnDiskIsInTheRegistry` fails. Launching
  OloEditor once rescans and adds it; commit the updated `.oar`.
* **Check the model's forward axis before trusting a rotation sign.** The Kenney
  vehicle models face +Z, matching the engine's vehicle convention, which is why
  `VehiclesTest.olo` gives them no yaw correction — but a model authored facing
  -Z (the usual glTF/Blender export) would need one, and then every hinge angle
  copied onto it reads mirrored.

## A targeted `--target OloEditor` build ships without C# scripting if one dependency edge goes

**Rule:** keep the `OloEditor → OloEngine-ScriptCore` / `Sandbox-Scripting` dependency edge in the
root `CMakeLists.txt`, next to the `OloRuntime` edge and inside the same
`if(CMAKE_GENERATOR MATCHES "Visual Studio")` block.

`OloEngine-ScriptCore.dll` builds straight into `OloEditor/Resources/Scripts/`. Without the edge the
DLL is never built, `ScriptEngine::Init` fails to load it, logs
`[ScriptEngine] OloEngine-ScriptCore assembly unavailable`, and disables C# scripting for the
session. That is graceful degradation, not a crash, so a verification loop that only checks for a
rendered window misses it. If you see that log line, check the edge before diagnosing anything else.
Found by a `/start-work` runtime smoke test, not a tracked issue.

## `.ply` is already claimed by the mesh importer, so a Gaussian-splat PLY fails as a broken mesh

**Rule:** before adding an importer for a format, grep `AssetExtensions.cpp` for the extension.
Registering a *second* meaning for one already in `s_ExtensionMap` is not a new capability, it is a
conflict, and the existing meaning wins — the file is routed to the old importer and the user is
told whatever that importer thinks of it.

`s_ExtensionMap["ply"] = AssetType::MeshSource` (added so the multi-million-triangle scanning-repo
meshes could be imported at all). A 3D Gaussian-splat `.ply` is also a `.ply`, so dropping one into
the Content Browser routes it to assimp, which **refuses it**:

```
ASSIMP Error: Validation failed: Mesh  contains no faces
```

`Model::LoadModel` then returns with no model. Measured against the #971 fixture with the engine's
own import flags; dropping `aiProcess_ValidateDataStructure` does not change the outcome, only the
message (*"No output meshes: all meshes are orphaned and are not referenced by any nodes"*), because
a splat cloud is vertices with no faces and the mesh path has nothing to keep.

**So the failure mode is a confusing error, not silent data loss.** That distinction matters for how
much it is worth fixing: nothing is imported wrongly, and no data is quietly dropped — a user is
told their file is a broken mesh when it is in fact a perfectly good file of a kind the engine does
not support yet. Worth a clearer message when splats become a real asset type, not before.

Found during the #971 splat viability spike, whose importer therefore stays out of the asset
registry entirely — see [ADR 0018](../adr/0018-gaussian-splats-gpu-ordering-and-merge-lod.md) §4 and
§6. The general shape applies to any format with a shared container extension: `.ply`, `.json`,
`.bin`, `.dat`. If both meanings must coexist, the discriminator has to be file CONTENT (for splat
PLYs, an `f_dc_0` property in the vertex element), not the extension.

**The wider lesson is about the write-up, not the code.** The first version of this note claimed the
splat PLY imported as a point mesh and silently lost everything but `x y z`. That was inferred from
reading `AssetExtensions.cpp` and never tested; running the file through assimp with the engine's
flags took about ten minutes and produced the opposite answer. A claim about what an importer *does*
is cheap to verify and easy to get backwards.

## An asset serializer that resolves a second asset deadlocks the whole editor, silently

`AssetImporter::TryLoadData` looks the serializer up in a registry guarded by an `FMutex`. That
mutex is **not recursive**. Until #646 it was held for the entire duration of the serializer call,
so any serializer that resolved a dependent asset re-entered the same function and blocked forever:

```
Scene::RenderTilemaps
  -> AssetManager::GetAsset<Tileset>
    -> AssetImporter::TryLoadData          <- takes the registry mutex
      -> TilesetSerializer::TryLoadData
        -> AssetManager::GetAsset<Texture2D>
          -> AssetImporter::TryLoadData    <- same mutex, blocks forever
```

**The symptom gives you nothing.** No crash, no assert, no log line — the editor window stays on the
shader-warmup title, the log simply stops mid-frame, and every automated capture reads as "black
screen". It looks exactly like a slow shader warmup on a loaded box, which is what it was mistaken
for first. The tell is that the log's last line is a render-pass entry and nothing follows it, ever.

The lock scope is now narrowed to the lookup itself — the registry is a function-local static
populated once by `Init()` and never mutated (`Shutdown()` is an intentional no-op), so the
serializer pointer stays valid after the lock drops. `MeshSerializer::TryLoadData` had been
resolving its `MeshSource` under exactly this hazard and was one dependency-ordering accident away
from the same freeze.

Two rules follow:

- **Prefer not to resolve a dependent asset inside `TryLoadData` at all.** Take what you need from
  the caller instead. `TilesetSerializer` used to resolve its atlas texture just to cache the pixel
  size; the render path was changed to slice against the texture it is already about to sample
  (`Tileset::GetTileUVForAtlas`), which removed the dependency *and* fixed a staleness bug when an
  atlas is re-exported at a different size.
- **If you must, know that the lock no longer protects you from ordering.** A cycle between two
  serializers is now infinite recursion instead of a deadlock. Neither is nice; don't build one.

**Debugging this shape:** attach `cdb` to the live process and dump the main thread — the nested
`AssetImporter::TryLoadData` frames name the culprit immediately. See
[windbg/cdb on this box](../../CLAUDE.md) for the package path; a `~0k 40` on the hung editor is a
30-second answer to a bug that is otherwise invisible.

Found on #646 (2D tilemap / tileset system).


## The MCP viewport override is in PIXELS, so it must not be DPI-scaled again

**Rule:** `m_ViewportSize` in `EditorLayer` is normally the ImGui panel size in LOGICAL points and
is multiplied by `Window::s_HighDPIScaleFactor` to get the framebuffer size. The MCP viewport
override (`olo_viewport_set_size`, #316) writes *pixels* into that same field, so the DPI multiply
has to be skipped while an override is active — otherwise the render target is the requested size
times the display scale.

On a 150% display that turned a requested 1600x900 into a **2400x1350** scene band, which is larger
than the window it has to fit. The render graph then could not service the scene band at all, and
every consumer of scene colour/depth resolved to null on that frame **and every frame after it**:
GTAO published fully-visible AO, `AOApplyRenderPass` hit its
`OLO_CORE_ASSERT(false, "…enabled without resolved graph input/output")` and killed the Debug
editor, and with that assertion removed the viewport simply went black with a warning per pass per
frame. The same multiply is applied on an ordinary window resize, which is why it first looked like
"resizing the editor crashes it" rather than an MCP-only fault.

Two things worth carrying forward:

- **A smaller override hides it.** 800x450 becomes 1200x675, still inside the window, and renders
  perfectly. Any test of this path has to use an override large enough to exceed the window, or it
  proves nothing.
- **Do not "fix" it by deleting the assertion.** That was tried first and is strictly worse: the
  crash becomes a permanently black viewport with a warning flood, which is the same failure with
  the loudest signal removed. The assertions in the post chain (AOApply, SSR, SSGI, ContactShadow,
  Bloom) are deliberate tripwires for "the graph did not give me what I need"; the bug was upstream
  of all of them.

Found on #1057, fixed in `EditorLayer::OnImGuiRender`'s framebuffer-size computation.

## Three of the "graph panels" are not canvases — rank by what a panel draws, not by line count

Before migrating a panel onto `EditorUI::GraphCanvas`, check that it *has* a canvas. Issue #1070
listed seven panels to migrate. Three of them draw no viewport maths at all, so there was nothing
to migrate and the honest outcome for each was a comment, not a PR. **The migration is finished:**
`ShaderGraphEditorPanel` landed in #1103 and the three real canvases below in #1070, so every panel
that has a viewport now shares the widget. The table is kept because the ranking mistake behind the
count of seven is the reusable part:

| Panel | Lines | Pan | Zoom | Grid | Bezier wire | What it actually is |
|---|---|---|---|---|---|---|
| `SoundGraphEditorPanel` | 2515 | yes | yes | yes | yes | a real canvas, migrated in #1070 |
| `DialogueEditorPanel` | 2042 | yes | yes | yes | yes | a real canvas, migrated in #1070 |
| `SkillTreeEditorPanel` | 1406 | yes | yes | yes | yes | a real canvas, migrated in #1070 |
| `AnimationGraphEditorPanel` | 990 | no | no | no | no | tabbed form; states are `ImGui::Selectable` rows |
| `FSMEditorPanel` | 114 | no | no | no | no | read-only `TreeNode` inspector |
| `BehaviorTreeEditorPanel` | 104 | no | no | no | no | read-only `TreeNode` inspector |

`AnimationGraphEditorPanel`'s single `ImDrawList` use is a 1D blend-space bar anchored to the ImGui
cursor — a progress bar with tick marks, not a node graph. `FSMEditorPanel` and
`BehaviorTreeEditorPanel` list entities carrying a component and print their blackboards; neither
edits anything despite the name.

Run three greps for the census: a panel with a private canvas declares a zoom, a panel with wires
calls `AddBezierCubic`, and a migrated panel names the widget.

```bash
grep -rln -E "m_[A-Za-z]*Zoom" OloEditor/src/     # now GraphCanvas + AnimationPanel only
grep -rln "AddBezierCubic" OloEditor/src/         # now GraphCanvas.cpp alone
# --include, or this one also matches GraphCanvas.{h,cpp} — the widget's own files:
grep -rln --include='*EditorPanel.*' "GraphCanvas" OloEditor/src/Panels/   # all five consumers
```

Those are still the census, and the `AddBezierCubic` grep is the decisive one: a new panel there
grew a private wire-rendering path instead of using the widget. A hit in the zoom grep alone is not
evidence -- `AnimationPanel` is a standing false positive (below) -- so check what that panel draws
before believing it.

**`AnimationPanel` is the one false positive in the zoom grep**, and it is not in #1070's list at
all: its `m_TimelineZoom` scales a 1D keyframe timeline (a playhead and tick marks, `AddLine`
only — no bezier, no 2D pan, no grid). Treat the `AddBezierCubic` grep as the decisive one; a
zoom on its own only means the panel scales *something*.

**Why the count was wrong.** Issue #1070's table (and the context section of
[ADR 0014](../adr/0014-visual-script-execution-model.md), which it inherited) ordered the panels by
`.cpp` line count and assumed every panel named `*EditorPanel` over a graph-shaped asset drew one.
Line count tracks how much *form* a panel puts on screen, not whether it has a viewport. The three
that are not canvases are `Editor` panels over graph-shaped **data**, which is a different thing.
Migrating one anyway would mean *adding* a canvas to a panel that never had one. That is a new
feature rather than a migration, and it is the kind of change ADR 0014 set out to avoid.

### What the three migrations actually cost, and what they fixed

`GraphCanvas` needed **no new capability** for any of the three (#1070) — `WasRightClicked()`, added
for `ShaderGraphEditorPanel` in #1103, was the last missing piece, and `FitToBounds`, `ResetView`,
`DistanceToWire` and `DrawDirectionalWire` covered everything else. What each panel keeps is node
layout, hit-testing, selection and link semantics; what it loses is the grid, the two transforms,
pan/zoom and the bezier maths. Roughly 300 net lines went away across the three.

Three bug classes fell out of the move, and they are the ones to look for in any future panel that
still hand-rolls part of this:

- **A pin hit radius that scales with zoom is untargetable when zoomed out.** All three multiplied
  the pin radius by zoom, so a 5–6 px pin became under 2 px at the canvas' minimum zoom. Every hit
  radius now has a screen-pixel floor: `std::max(s_PinHitRadiusMin, m_Canvas.Scaled(r))`.
- **A context-menu position captured in SCREEN space is wrong by the time the popup draws.** All
  three stored the click position and converted it when the menu ran, so a node landed in the wrong
  place if the view moved in between. Capture `ToGraph(mousePos)` at click time instead.
- **A "fit the view" that measures the wrong rectangle.** `DialogueEditorPanel`'s Zoom to Fit read
  `ImGui::GetContentRegionAvail()` from inside the **menu bar**, and its Add Node menu passed the
  window position in as the canvas origin. Both are impossible to write once the canvas owns its
  own origin and size; use `FitToBounds` and `GetOrigin()`/`GetSize()`.

Deliberate behaviour changes, all of them the widget's rules rather than a panel's: right-drag pans
everywhere (so `DialogueEditorPanel` lost Alt+left-drag panning), zoom anchors on the cursor and is
multiplicative over [0.15, 3.0], wire thickness is screen pixels rather than `2 * zoom`, grid majors
land every 5 in graph space, and a context menu opens on right-**release**-without-drag.

## Use an explicit zero UUID for absent relationships

Initialize absent parent/asset references with `UUID(0)` or `{ 0 }`; `UUID{}` generates a
new identity. Test both the stored UUID and whether it resolves to an entity. During live
verification of #1126, adding a child constructed its parent's `RelationshipComponent`
with a random parent UUID. `GetParent()` returned an empty entity, so the existing cycle
test passed, while the hierarchy's zero-UUID root filter hid the whole branch and the
serializer persisted the dangling reference. The default now uses zero, and hierarchy
and automation round-trip tests assert that root identity directly.
