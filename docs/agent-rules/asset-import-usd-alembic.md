# Asset import: the interchange abstraction + OpenUSD / Alembic / MaterialX vendoring (#655)

How the USD / Alembic / MaterialX / glTF-export import breadth (issue #655) is wired, and every
non-obvious gotcha hit vendoring these three heavyweight VFX libraries into the static-everything,
FetchContent build. Read before touching `OloEngine/src/OloEngine/Asset/Interchange/` or the vendor
CMake for these deps.

## The interchange seam (Tier 2 abstraction)

Import no longer hard-codes Assimp. `MeshSourceSerializer::TryLoadData` dispatches through a
**`MeshImporterRegistry`** (`Asset/Interchange/MeshImporterRegistry.h`) keyed by lower-cased file
extension, with a **fallback importer**:

- `AssimpMeshImporter` is the fallback (and claims fbx/gltf/glb/obj/dae/vrm/ply) — it just wraps the
  existing `Model` + `CreateCombinedMeshSource` path, so FBX/glTF/OBJ/DAE/VRM/PLY are byte-for-byte
  unchanged (including the `.omesh` geometry cache).
- `UsdMeshImporter` / `AlembicMeshImporter` claim their own extensions, compiled in only under
  `OLO_WITH_USD` / `OLO_WITH_ALEMBIC`.
- Every importer returns a `Ref<MeshSource>` **without** calling `Build()` (the serializer builds;
  the headless/asset-pack path must not) and sets `SetPreOptimized(true)` when the combined buffer is
  already meshopt-shaped, exactly like `Model::CreateCombinedMeshSource`.

Export is a **separate axis**: `MeshExporterRegistry` + `AssimpMeshExporter` (glTF/glb via the
already-vendored assimp `Exporter`, format ids `gltf2`/`glb2` — **no new dependency**). Assimp's
exporters are ON by default in this repo's config (nothing sets `ASSIMP_NO_EXPORT`).

Extensions register in `AssetExtensions.cpp` gated on the `OLO_WITH_*` PUBLIC compile definitions, so
an unbuilt format never mis-maps (`.abc`/`.usd*` → MeshSource, `.mtlx` → Material). `.mtlx` is routed
inside `MaterialAssetSerializer::TryLoadData` (it's XML, not the engine's YAML material format).

## OpenUSD — the big one (`OLO_WITH_USD`, **default ON**, config-matched auto-build)

USD import is **on by default** (matching UE5's USD-by-default posture). `cmake/vendor/OpenUSD.cmake`
resolves USD one of two ways:
- **Prebuilt** — `-DOLO_USD_INSTALL_DIR=<prefix>` at an existing static-monolithic USD + oneTBB
  install uses it directly (a flat single-config install; CI caches / a shared build).
- **Auto-build (default)** — otherwise oneTBB + static-monolithic OpenUSD build **from source** via
  `ExternalProject_Add`, ONCE, into a per-user version-keyed cache (`$LOCALAPPDATA/OloEngine/usd-<sha8>`
  on Windows). To dodge the Windows CRT trap (USD returns `std::string` — a cross-CRT free crashes),
  **both Debug and Release are built** into per-config prefixes (`install/Debug`, `install/Release`);
  the engine links the CRT-matching one via a `$<CONFIG>` genexpr (Debug engine → Debug USD;
  Release/RelWithDebInfo/MinSizeRel → Release USD; Debug oneTBB is `tbb12_debug.lib`, Release is
  `tbb12.lib`). First build ≈ 30–45 min + ~10 GB; later builds are machine-wide cache hits.
  **CI that doesn't need USD sets `-DOLO_WITH_USD=OFF`.**

### The cache is shared across worktrees — the stamps are not (fixed; know the failure it caused)

The cache holds the source/binary/install trees, but `ExternalProject`'s `STAMP_DIR` defaults to the
**consuming** build tree. So a **second worktree** used to see no stamps, conclude every step was out
of date, and re-run configure+build *inside the first worktree's* `usd-build` — which is not
idempotent, because `BUILD_COMMAND` is `--config Release` **then** `--config Debug` in one binary dir.
Re-entering a finished tree fails across USD's `arch` target with:

```text
error C2859: ...\arch.dir\Release\arch.pdb is not the pdb file that was used when this
             precompiled header was created, recreate the precompiled header.
```

The artifacts were shared; the "already done" record was not. **Symptom to recognise:** a *fresh
worktree* failing in `olo_openusd` while `install/{Debug,Release}` is demonstrably complete
(`usd_m.lib` ≈ 2.4 GB Debug / 2.1 GB Release).

`OpenUSD.cmake` now **short-circuits on a complete cache**: if `usd_m` + the config's oneTBB lib +
`include/pxr` + the `lib/usd` plugInfo tree all exist for **both** configs, it skips
`ExternalProject_Add` entirely (`olo_usd_ext` becomes a no-op) and consumes the cache as a prebuilt
install — so no worktree after the first ever writes to the shared tree. A *partial* cache (an
interrupted first build) fails the check and resumes the real build. Bumping `_OLO_USD_TAG` changes
the cache key; to force a genuine rebuild, delete `$LOCALAPPDATA/OloEngine/usd-<sha8>`.

Two things that follow:

- **Moving `STAMP_DIR` into the cache would NOT have fixed this** — it only relocates the race to two
  worktrees configuring for the first time simultaneously.
- **Still unsafe: two worktrees running their FIRST USD build concurrently** (one shared binary dir).
  Same rule as the msvc/clangcl trees in
  [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) — sequence the first build; every
  tree after it is a lock-free cache hit.

`-DOLO_USD_INSTALL_DIR` expects a **flat** prefix and now **probes** for `tbb12` vs `tbb12_debug`
rather than assuming release. It previously hardcoded `tbb12.lib`, so pointing it at a Debug prefix
(e.g. one config of the cache above) configured cleanly and then failed at link on a missing lib —
which made the documented escape hatch useless for exactly the Debug build that hit the C2859 above.

**Do NOT vendor OpenUSD via FetchContent `add_subdirectory`.** USD's CMake is install-centric: the
runtime `plugInfo.json` resource tree it needs to open *even a plain `.usda`* is produced by the
**install step**, and its `pxr_*` macros pollute the parent project — hence ExternalProject
(build + install out-of-tree), not a subproject.

The ExternalProject mirrors this standalone superbuild recipe (also handy for producing an
`OLO_USD_INSTALL_DIR` prebuilt):

```cmake
# oneTBB (USD's ONE mandatory dep; no Boost/Imath/zlib for core USD)
ExternalProject_Add(onetbb GIT_REPOSITORY .../oneTBB.git GIT_TAG v2022.2.0 GIT_SHALLOW TRUE
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<prefix> -DBUILD_SHARED_LIBS=OFF -DTBB_TEST=OFF -DTBB_STRICT=OFF)
# OpenUSD static monolithic (usd_m), headless, data-import only
ExternalProject_Add(openusd DEPENDS onetbb GIT_REPOSITORY .../OpenUSD.git GIT_TAG v25.11 GIT_SHALLOW TRUE
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<prefix> -DTBB_DIR=<prefix>/lib/cmake/TBB
    -DBUILD_SHARED_LIBS=OFF -DPXR_BUILD_MONOLITHIC=ON -DPXR_ENABLE_PYTHON_SUPPORT=OFF
    -DPXR_BUILD_IMAGING=OFF -DPXR_BUILD_USD_IMAGING=OFF -DPXR_BUILD_USDVIEW=OFF
    -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF
    -DPXR_BUILD_DOCUMENTATION=OFF -DPXR_BUILD_USD_TOOLS=OFF -DPXR_ENABLE_GL_SUPPORT=OFF
    -DPXR_ENABLE_MATERIALX_SUPPORT=OFF -DPXR_ENABLE_OPENVDB_SUPPORT=OFF -DPXR_ENABLE_OSL_SUPPORT=OFF
    -DPXR_ENABLE_PTEX_SUPPORT=OFF -DPXR_ENABLE_HDF5_SUPPORT=OFF -DPXR_PREFER_SAFETY_OVER_SPEED=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
```

### Gotchas that actually bit (all confirmed on VS 18 2026 / MSVC 19.51 / CMake 4.2):

1. **`MAX_PATH` on the clone.** oneTBB (and USD) have deeply-nested doc files that overflow Windows'
   260-char path limit during `git clone`. `git config --global core.longpaths true` **and** use a
   very short build-tree base path (this build used `D:/ou`), not a deep scratchpad path.
2. **Static-monolithic PDB-install bug.** USD's `cmake/macros/Public.cmake` unconditionally does
   `install(FILES $<TARGET_PDB_FILE:usd_m>)` on WIN32, but `usd_m` is a **static** archive (no linker
   PDB) → generate-time error *"TARGET_PDB_FILE is allowed only for targets with linker created
   artifacts"*. Patch the guard to `if(WIN32 AND BUILD_SHARED_LIBS)` (an ExternalProject `PATCH_COMMAND`
   for the committed superbuild). Without it the static build never generates.
3. **No build-time Python needed.** Schema `.cpp` are pre-generated and checked in; with
   `PXR_ENABLE_PYTHON_SUPPORT=OFF` the build logs *"Skipping building usdGenSchema, Python modules
   required"* and compiles fine. Boost is gone from core USD since v24.11.
4. **The static-plugin runtime trap.** `.usda`/`.usdc`/`.usdz` are **plugin-registered**, so even a
   static monolithic build needs the installed `lib/usd/**/plugInfo.json` tree at runtime AND the
   objects force-linked. `UsdMeshImporter` calls `PlugRegistry::RegisterPlugins` on
   `OLO_USD_PLUGIN_PATH` (env, for the headless smoke test) or `<exe>/usd` (the tree CMake stages next
   to the editor). The engine link force-links the archive with `/WHOLEARCHIVE:` or the Sdf/schema
   static-init registrations are dropped and `UsdStage::Open` returns null. Pass it the **full
   path**, not the bare `usd_m` — see gotcha 7.
5. **Cost.** `usd_m.lib` ≈ **2.1 GB** Release / **2.4 GB** Debug (static archive — dead-stripped at
   the final exe link). Cold build ≈ 8–15 min per config. Config-matched builds both.
6. **oneTBB static-Debug teardown assert.** A static Debug oneTBB trips a benign worker-teardown
   assert (`small_object_pool.cpp:143`, `m_private_counter >= 0`) that aborts the process at exit —
   all work completes, but it would pop an assert dialog on every Debug editor close. Fixed by
   building oneTBB with `-DCMAKE_CXX_FLAGS=-DTBB_USE_ASSERT=0` (the `#ifndef TBB_USE_ASSERT` guard
   respects a command-line define; disables `__TBB_ASSERT` in the compiled lib; Release already has
   it off via `NDEBUG`). Verify a fix took by grepping the assert string out of `tbb12_debug.lib`
   AND clean-relinking the consumer (MSBuild won't relink on a same-path `.lib` content change).
7. **`/WHOLEARCHIVE:` needs a FULL PATH — a bare library name is not portable across the two
   Windows linkers** (#697). `link.exe` resolves a bare `/WHOLEARCHIVE:usd_m` against `/LIBPATH`;
   **`lld-link` does not** — it opens the argument as a path relative to the working directory only,
   and kills the whole link with

   ```text
   lld-link: error: could not open 'usd_m': no such file or directory
   ```

   even though `usd_m.lib` is already on the link line by full path and its directory is in
   `/LIBPATH`. This is not a niche path: **CMake sets `MSVC` TRUE for clang-cl** (it targets the MSVC
   ABI), so an `if(MSVC)` guard is taken under the `clangcl` / `clangcl-asan` presets too. The result
   was that both of those presets could not link *at all* with the default `OLO_WITH_USD=ON`, while
   the msvc preset was perfectly green — everything compiled, so it looked like a link-config problem
   rather than an option-spelling one.

   The fix is to spell the option with the full path CMake already has
   (`target_link_options(OloEngine PUBLIC "/WHOLEARCHIVE:${OloEngine_USD_LIB}")`), which is accepted
   by **both** linkers. Verified against **LLD 21.1.8** and **MSVC `link.exe` 14.51** with a minimal
   force-link probe (a static lib whose only content is a static-init side effect, and a `main` that
   never references it):

   | `/WHOLEARCHIVE:` argument                   | `link.exe` | `lld-link` |
   | ------------------------------------------ | ---------- | ---------- |
   | bare name, lib reachable via `/LIBPATH`     | force-links | **link fails** |
   | full path, lib also on the link line        | force-links | force-links |
   | full path, lib *not* on the link line       | force-links | force-links |
   | full path containing spaces                 | force-links | force-links |
   | *(control)* no `/WHOLEARCHIVE` at all       | object dropped | object dropped |

   The control row is the part worth keeping: it is what proves the probe measures force-linking
   rather than incidental reachability.

   Do **not** "fix" a `/WHOLEARCHIVE` failure by deleting the option. It is load-bearing under both
   linkers, but in *different* ways — measured by relinking the engine with the option commented out:
   under `link.exe` the link succeeds and USD fails at **runtime** (gotcha 4), while under `lld-link`
   the link fails outright with
   `undefined symbol: __declspec(dllimport) ... UsdGeomGetStageUpAxis` — because `usd_m`'s headers
   declare its symbols `dllimport` (the `/IGNORE:4217` flood) and lld-link will not satisfy a
   dllimport-declared symbol from the archive without the whole-archive pull. So "it linked after I
   removed the flag" is a *link.exe-only* observation, and it is the silent-at-link,
   broken-at-runtime case.

   `OloEngine_USD_LIB` carries a `$<CONFIG>` genexpr and `target_link_options` expands
   it per config, so the full-path spelling is also config-matched where the bare name depended on
   `/LIBPATH` search order to land on the right one.

Silent-correctness handling in `UsdMeshImporter`: stage up-axis (Z-up → -90° X rotation),
`metersPerUnit` scale, `orientation` (leftHanded → reversed winding), primvar interpolation
(constant/uniform/vertex/faceVarying) indexing incl. indexed primvars, `st` V-flip to the glTF
top-left convention. First slice: static mesh, one submesh per mesh prim (GeomSubset per-face-group
materials, skinning, point instancers, usdz-embedded textures are follow-ups).

## Alembic (`OLO_WITH_ALEMBIC`, default ON, FetchContent + Imath)

FetchContent Imath **before** Alembic so Alembic's `IF(TARGET Imath::Imath)` short-circuits its
`find_package(Imath)`. Ogawa-only (`USE_HDF5=OFF`), static, no Boost. Two gotchas:

1. **`IMATH_INSTALL` must be ON.** Alembic's `lib/Alembic/CMakeLists.txt` does an *unconditional*
   `export(TARGETS Alembic)` + `install(EXPORT AlembicTargets)`, both referencing `Imath::Imath`; with
   `IMATH_INSTALL OFF` Imath is in no export set → *"requires target Imath that is not in any export
   set"* at generate. Export sets are only **validated** at generate and only **executed** on
   `cmake --install` (which this repo never runs), so `IMATH_INSTALL ON` is validation-only — nothing
   is actually installed. `CMAKE_SKIP_INSTALL_RULES` does **not** fix this (it doesn't affect the bare
   `export()`).
2. **`<Imath/half.h>` not found.** The FetchContent'd Imath target only puts `src/Imath` on the include
   path (so a flat `<half.h>` resolves), but Alembic includes `<Imath/half.h>` and bakes the
   `${Imath_INCLUDE_DIRS}` *variable* (unset without find_package) into its PUBLIC includes. Split the
   MakeAvailable and set `Imath_INCLUDE_DIRS = $<BUILD_INTERFACE:${imath_SOURCE_DIR}/src>` between
   them. **Must** be wrapped in `$<BUILD_INTERFACE:>` — a bare absolute source path in a PUBLIC
   `INTERFACE_INCLUDE_DIRECTORIES` is a CMake policy error.

`AlembicMeshImporter` reads IPolyMesh/ISubD rest pose only (multi-sample animation logged as a
follow-up). All transform math stays in Imath `M44d` (row-vector) to sidestep the glm column-vector
mismatch. Handles facevarying-vs-vertex normal/UV indexing (the classic Alembic bug), geometric-normal
fallback (Newell), and V-flip. Alembic carries no PBR material → one engine-default per import.

## MaterialX (`OLO_WITH_MATERIALX`, default ON, FetchContent)

Clean static `add_subdirectory`; link **MaterialXCore + MaterialXFormat** only. The trap is that
`MATERIALX_BUILD_RENDER` and every `MATERIALX_BUILD_GEN_*` (incl. the newer `_GEN_SLANG`) default
**ON** — turn them all OFF or you drag in the whole shader-gen stack. PugiXML is bundled into
MaterialXFormat; the feared double-pugixml LNK2005 with assimp's bundled copy **did not occur** in
this repo's static link (watch for it on a toolchain bump). Reading authored input values needs **no**
stdlib `libraries/` tree at runtime.

`MaterialXMaterialReader` maps `standard_surface` / `UsdPreviewSurface` / `gltf_pbr` (branches on
`node->getCategory()` — the input names differ: `base_color`/`metalness`/`specular_roughness` vs
`diffuseColor`/`metallic`/`roughness`) to the PBR factors. Textures are resolved + logged; wiring them
through the asset manager (needs GL) is a follow-up. The mapper is a free function so a future
USD-with-MaterialX path can reuse it.
