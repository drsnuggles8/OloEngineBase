#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <glm/glm.hpp>

#include <atomic>
#include <span>
#include <string>
#include <vector>

namespace OloEngine
{
    class Material;

    // Settings for one scene lightmap bake (issue #439). The defaults are sized
    // for a small sandbox scene; tests use much smaller atlases and sample counts.
    struct LightmapBakeSettings
    {
        u32 AtlasSize = 1024;      // scene atlas dimension (square, power of two)
        u32 MinRegionSize = 8;     // per-entity region floor (power of two)
        u32 SamplesPerTexel = 128; // Monte Carlo samples per lightmap texel
        u32 MaxBounces = 4;        // surface interactions per path (1 = single bounce)
        f32 TexelsPerMeter = 8.0f; // world-space texel density target
        u32 DilationPasses = 4;    // chart-edge dilation iterations after the bake
        u32 Seed = 0x439u;         // global bake seed (texel seeds derive from it)
        // NOTE: the per-mesh unwrap parameters are deliberately NOT settings.
        // Prepare() always unwraps with the shared constants
        // (kLightmapUnwrap{Resolution,Padding} in LightmapUnwrap.h) because the
        // runtime's self-healing re-unwrap (SceneLightmapRuntime::Resolve)
        // hard-codes the same constants — a bake with different values would
        // produce UV2 the resolve can never reproduce, silently mis-addressing
        // every lightmap sample (the bake key hashes counts/bounds, not UV
        // values, so it would not catch the mismatch).

        // The staleness key the caller computed over the scene state this bake
        // captures (static entities, lights, environment, settings). Stored in
        // the asset verbatim; the runtime refuses to sample when it no longer
        // matches the live scene. 0 = caller did not provide one.
        u64 BakeKey = 0;
    };

    // One static entity to bake. Gathered by the caller (the Scene-side wiring
    // decides what "static" means); the baker never touches the ECS.
    struct LightmapBakeInput
    {
        u64 EntityUUID = 0;
        Ref<MeshSource> Mesh;
        glm::mat4 WorldTransform = glm::mat4(1.0f);
    };

    struct LightmapBakeResult
    {
        bool Success = false;
        std::string Error;        // set when Success == false ("cancelled" included)
        Ref<LightmapAsset> Asset; // set when Success == true
        u32 BakedEntityCount = 0;
        u32 SkippedEntityCount = 0; // atlas exhaustion / unwrap failure — listed in the log
    };

    // One texel awaiting an irradiance estimate: its atlas coordinates (the
    // seed source — position-derived, so thread scheduling cannot change a
    // texel's value) and the surface point it shades.
    struct LightmapTexelJob
    {
        u32 AtlasX = 0;
        u32 AtlasY = 0;
        glm::vec3 WorldPos{ 0.0f };
        glm::vec3 WorldNormal{ 0.0f };
    };

    // One entity's square atlas region in texel coordinates. Dilation is
    // restricted to the owning rect so one entity's lighting never bleeds
    // into a neighbouring entity's border texels.
    struct LightmapAtlasRegion
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Size = 0;
    };

    // The game-thread product of Prepare(): everything the background texel
    // bake needs, with no reference back to any MeshSource. Immutable once
    // handed to BakeTexels — that is what makes the split thread-safe (the
    // unwrap MUTATES meshes and must run where the editor renders them; the
    // texel loop reads only this struct and the ReferenceScene).
    struct LightmapBakePrepared
    {
        std::vector<LightmapTexelJob> Jobs;
        std::vector<LightmapEntityEntry> Entries;
        std::vector<LightmapAtlasRegion> Regions; // parallel to Entries (same order)
        u32 AtlasSize = 0;
        u32 BakedEntityCount = 0;
        u32 SkippedEntityCount = 0;
    };

    // CPU lightmap baker (issue #439). The bake kernel is the reference path
    // tracer: each texel's stored value is PathTracer::EstimateIrradiance at the
    // texel's world position/normal — cosine-hemisphere INDIRECT irradiance E in
    // physical units (delta lights never appear in the estimator except through
    // bounces, so punctual direct lighting is excluded by construction and stays
    // realtime). See docs/agent-rules/reference-path-tracer.md for the oracle's
    // contract; the bake inherits its determinism (texel seeds derive from atlas
    // coordinates, so thread scheduling cannot change a single texel).
    //
    // Runs headless — no GL. Safe to call from a background thread; `world`'s
    // const queries are thread-safe once built. Mutates the input MeshSources
    // (generates their lightmap UV stream in place when absent); callers must
    // re-Build() the affected meshes on the render thread afterwards.
    class LightmapBaker
    {
      public:
        LightmapBaker() = delete;

        // Stage 1 — GAME THREAD. Unwraps every input mesh (in place; caller
        // re-Build()s them afterwards so rendering picks up the seam-split
        // vertex data), sizes and packs atlas regions, and rasterizes every
        // chart into texel jobs. Entities with a non-finite or singular world
        // transform are skipped with a warning (a NaN would defeat every
        // downstream degeneracy check). Returns false with `outError` set on
        // failure.
        [[nodiscard]] static bool Prepare(std::span<const LightmapBakeInput> entities,
                                          const LightmapBakeSettings& settings,
                                          LightmapBakePrepared& outPrepared,
                                          std::string& outError);

        // Stage 2 — ANY THREAD. Reads only `prepared` and `world` (whose const
        // queries are thread-safe once built); one EstimateIrradiance per texel,
        // then dilation and asset assembly. `world` must already be Build()t and
        // should contain the same static geometry/lights the inputs described
        // (use ReferenceSceneBuilder). `progress` (0..1) and `cancelToken`
        // follow the AssetPackBuilder convention; both may be null.
        [[nodiscard]] static LightmapBakeResult BakeTexels(const LightmapBakePrepared& prepared,
                                                           const PathTracing::ReferenceScene& world,
                                                           const LightmapBakeSettings& settings,
                                                           std::atomic<f32>* progress = nullptr,
                                                           const std::atomic<bool>* cancelToken = nullptr);

        // Convenience: Prepare + BakeTexels on the calling thread (tests,
        // headless tools). The editor uses the two stages separately.
        [[nodiscard]] static LightmapBakeResult Bake(std::span<const LightmapBakeInput> entities,
                                                     const PathTracing::ReferenceScene& world,
                                                     const LightmapBakeSettings& settings,
                                                     std::atomic<f32>* progress = nullptr,
                                                     const std::atomic<bool>* cancelToken = nullptr);
    };
} // namespace OloEngine
