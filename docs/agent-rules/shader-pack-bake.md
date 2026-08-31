# The CI-baked shader pack (issue #908)

A packaged game still cold-compiled every shader on first launch even after
#906 gave the engine a content-hashed SPIR-V cache — a fresh machine has no
warm cache to hit. `ShaderPack` (`OloEngine/src/OloEngine/Renderer/ShaderPack.*`)
existed already as a name-keyed `.osp` archive of pre-compiled SPIR-V, built by
hand from the editor's "Build Shader Pack" menu item
(`EditorLayer::BuildShaderPack`). This issue made it CI-baked, content-hash
validated, and something `GameBuildPipeline` actually ships.

## The invalidation contract

A pack entry is now keyed by a **content hash**
(`OpenGLShader::ComputeContentHash` / `ComputeContentHashFromSources`), not by
name alone. The hash is computed from the preprocessed source (per stage,
includes already resolved) plus **both** compiler-option descriptors the two
SPIR-V tiers a pack entry stores actually depend on:
`Utils::VulkanTierOptions::Descriptor()` (the Vulkan-tier SPIR-V) and
`Utils::OpenGLTierOptions::Descriptor()` (the OpenGL-cross-compiled SPIR-V,
via SPIRV-Cross). Two callers that get the same hash string back are
guaranteed to compile to the same bytes for both tiers — a mismatch is
unambiguously a miss, never a "maybe stale".

`ShaderLibrary::TryReadPackEntry` recomputes the hash from the CURRENT
on-disk source before ever calling `ShaderPack::LoadEntry`, and only serves
the pack entry when it matches `ShaderPack::GetContentHash(filepath)`. A
mismatch (or a pack that predates this contract — see the version bump below)
logs an `OLO_CORE_WARN` and falls back to the normal compile path, exactly
like any other pack miss — not silent, just not fatal.

`SHADER_PACK_VERSION` bumped from 1 to 2 for the new on-disk field; a v1 pack
fails the version check outright rather than being half-read.

## The headless producer — no GL context

`ShaderPack::CreateFromFilepaths` (and the shared `WritePackFile` writer it
factors out of `CreateFromLibraries`) uses only `Shader::PrepareBatch` — read,
preprocess, shaderc, SPIRV-Cross — the CPU-only half of shader compilation.
No GL context is created, touched, or required anywhere in this path, which is
what lets the bake run on an ordinary hosted CI runner instead of needing the
self-hosted GPU box `gpu-conformance-amd.yml` uses.

The producer enumerates `Renderer2D::GetShaderFilepaths()` +
`Renderer3D::GetShaderFilepaths()` — the SAME arrays `Renderer2D::Init()` /
`Renderer3D::Init()` load at runtime (`kShaderPaths2D` / `kShaderPaths3D`,
each defined once and read by both the real Init() and this producer), so the
bake can never enumerate a different shader set than what the pack will
actually be asked to serve. It does **not** glob-scan `assets/shaders/` —
compute shaders and other subsystems' shaders (SSAO, SSR, DDGI, …) load
through their own `ShaderLibrary` instances that don't consult the pack at
all; packing them would be dead weight with no consumer.

The Lua scripting glue (`LuaScriptGlue.cpp`) also exposes
`Renderer2D`/`Renderer3D`'s `ShaderLibrary.Load(filepath)` to scripts, so a
game can load a filepath the fixed `kShaderPaths2D`/`kShaderPaths3D` arrays
never enumerate — that shader is never packed and always cold-compiles on
first launch, even with a pack staged. Not a correctness bug (a pack miss
falls back to compiling from source, same as any other miss), just a coverage
gap worth knowing about before promising a packaged build "will not compile
shaders from source on first launch" — that guarantee only covers the fixed
engine set.

`ShaderPack::CollectShaderFilepaths` (a directory-glob helper, skipping
`include/` and `tests/`) exists for completeness and is exercised in tests,
but is not what the CI producer uses today — see the note above.

## CI wiring

`.github/workflows/Windows.yml`, after the normal test run: the test binary's
own `--olo-bake-shader-pack=<path>` mode (see `TestOptions.h`) runs just the
`ShaderPackBakeTest.BakeWhenRequested` case via `--gtest_filter`, then the
result is uploaded as a `ShaderPack` build artifact.

## Consumption

`Renderer2D::Init()` / `Renderer3D::Init()` call
`m_ShaderLibrary.LoadShaderPack("assets/ShaderPack.osp")` when that file
exists, before their normal shader-load calls — a missing file is silently a
no-op (every `Load()` falls back to compiling from source, same as always).

`GameBuildPipeline::CopyEngineResources` stages the pack via the free function
`StageShaderPack` (mirrors `StageRuntimeDependencyLibraries` /
`StageLooseRuntimeTextures` — a small filesystem seam kept testable without a
full pipeline run) right after the existing `assets/shaders` source copy.
Staging is a no-op, not an error, when no pack exists next to the editor's
shaders — the same "optional" contract as the runtime load.

## Fresh worktrees do not fetch the pack automatically

A fresh `/start-work` worktree gets no `ShaderPack.osp` by default — nothing
downloads the CI artifact for it. This is deliberate (the issue's own plan
leans this way): the pack is a build-time optimization for a **packaged**
game, not something a working-tree editor session needs, and auto-fetching a
CI artifact into every worktree would need auth, versioning, and staleness
handling this issue doesn't need to solve. A game's build step (or an agent
producing a package) that wants the pack downloads the `ShaderPack` artifact
from the relevant Windows workflow run and drops it at
`OloEditor/assets/ShaderPack.osp` before running `GameBuildPipeline::Build`.
