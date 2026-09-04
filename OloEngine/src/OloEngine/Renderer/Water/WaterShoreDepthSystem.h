#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/Water/WaterShoreDepth.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    class Texture2D;

    /// One terrain tile as the seabed bake sees it (issue #1033).
    ///
    /// Deliberately NOT a TerrainComponent: the bake takes raw height fields so
    /// it can be exercised headlessly, with no ECS, no assets and no GL context.
    /// The caller (Scene) is what knows how to get from a component to these
    /// numbers, and that is the only place the two are joined.
    ///
    /// Heights are the engine's normalised [0, 1] convention, sampled by
    /// TerrainData::SampleHeight, so world Y = `BaseY + h * HeightScale`.
    ///
    /// Rotation is not represented. A rotated terrain tile would need its own
    /// inverse transform per texel and every terrain in this engine is authored
    /// axis-aligned; a rotated one bakes as though it were not, which shows as
    /// waves shoaling against the wrong coastline rather than as an error. If
    /// that becomes real, this struct grows a transform — do not paper over it
    /// at the sampling end.
    struct SeabedTerrain
    {
        glm::vec2 OriginXZ{ 0.0f };                ///< world XZ of the tile's minimum corner
        glm::vec2 SizeXZ{ 1.0f };                  ///< tile world size in metres
        f32 BaseY = 0.0f;                          ///< world Y of a normalised height of 0
        f32 HeightScale = 1.0f;                    ///< metres per unit of normalised height
        u32 Resolution = 0;                        ///< height field is Resolution x Resolution
        const std::vector<f32>* Heights = nullptr; ///< row-major, borrowed for the call
                                                   ///< (TerrainData::GetHeightData)
        /// TerrainData::GetHeightRevision(). Carried because the heights vector's
        /// ADDRESS is not an identity for its contents: a sculpt rewrites every
        /// sample in place, at the same address, so a bake keyed on the pointer
        /// alone would go on shoaling against the coastline the scene loaded with.
        u64 HeightRevision = 0;
    };

    /// What the scene asks the bake for.
    struct WaterShoreBakeRequest
    {
        glm::vec2 CentreXZ{ 0.0f }; ///< water surface's world-XZ centre
        f32 ExtentMetres = 0.0f;    ///< side length of the square field window
        f32 WaterPlaneY = 0.0f;     ///< world Y of the undisplaced water plane
    };

    /// Scene-level shore-wave controls, published each frame from the dominant
    /// WaterComponent — the same handoff shape WaterDisturbanceSettings uses.
    struct WaterShoreSettings
    {
        bool m_Enabled = false;
        /// The a/h limit the surf zone breaks at. WaterShore::kBreakerIndex
        /// (0.39) is the physical value; lowering it moves the breaker line into
        /// deeper water, which is an ART decision and is why it is authored
        /// rather than fixed.
        f32 m_BreakerIndex = WaterShore::kBreakerIndex;
        /// Multiplier on the breaking-wave foam. 0 keeps the geometry and drops
        /// the white water.
        f32 m_FoamGain = 1.0f;
        /// Camera distances over which the breaker foam fades. Far longer than
        /// the crest-foam fade for the reason WaterUBO::ShoreParams2 records.
        f32 m_FoamFadeStartMetres = 120.0f;
        f32 m_FoamFadeEndMetres = 400.0f;
    };

    // =========================================================================
    // WaterShoreDepthSystem — the renderer-owned seabed depth field (#1033).
    //
    // Resamples the scene's terrain height fields into ONE square RGBA16F field
    // covering the water surface, which the water displacement chain samples
    // once per vertex to shoal, refract and break its waves. The encoding
    // contract, and every relation the field drives, are in WaterShoreDepth.h.
    //
    // A static singleton for the same two reasons WaterDisturbanceSystem is one:
    // the field is a persistent texture, which is exactly what the render
    // graph's transient aliasing is wrong for, and keeping it out of the graph's
    // internals keeps it clear of the concurrent rework there.
    //
    // Unlike the disturbance field this is NOT updated per frame. The sea floor
    // does not move, so Rebuild() runs only when the scene says the inputs
    // changed (a terrain regenerated, an island moved, the water tile resized) —
    // see BuildSignature. A per-frame bake of six 512x512 height fields would be
    // pure waste, and a per-frame COMPUTE version of it would be waste plus a
    // cross-frame history resource nothing needs.
    //
    // The baked field is retained on the CPU, which is not an optimisation: it
    // is what lets WaterSurface's buoyancy sampler float boats on the SAME
    // shoaled surface the GPU draws, headlessly, with no readback.
    // =========================================================================
    class WaterShoreDepthSystem
    {
      public:
        /// Create the field texture. Safe to call without a GL context only in
        /// the sense that it will report failure and disable the feature.
        static void Init();
        static void Shutdown();
        [[nodiscard]] static bool IsInitialized();

        /// Publish this frame's appearance/behaviour knobs.
        static void SetSettings(const WaterShoreSettings& settings);
        [[nodiscard]] static const WaterShoreSettings& GetSettings();

        /// A cheap fingerprint of everything the bake reads. The scene keeps the
        /// last one and rebuilds only when it changes, which is what keeps this
        /// off the per-frame path without anyone having to remember to
        /// invalidate it.
        [[nodiscard]] static u64 BuildSignature(const WaterShoreBakeRequest& request,
                                                std::span<const SeabedTerrain> terrains);

        /// Resample the terrains into the field and upload it. Idempotent: a
        /// request whose signature matches the current one is a no-op, so a
        /// caller may hand this every frame's state without conditioning on it.
        static void Rebuild(const WaterShoreBakeRequest& request,
                            std::span<const SeabedTerrain> terrains);

        /// Drop the baked field (scene close / switch). The next Rebuild with
        /// any signature re-bakes.
        static void Invalidate();

        /// UBO packing. `w <= 0` is the disabled state and is reported for every
        /// reason the field could be unusable — not initialised, disabled by the
        /// scene, or never baked — so a caller that packs this unconditionally
        /// cannot put a stale field on screen.
        [[nodiscard]] static glm::vec4 GetShaderParams();
        /// x = breaker index, y = foam gain, z/w = foam fade start/end metres.
        [[nodiscard]] static glm::vec4 GetShaderParams2();

        [[nodiscard]] static RHI::ResourceHandle GetFieldTextureHandle();

        /// Stage the bindless heap offset AND issue the slot bind, for the same
        /// reason WaterDisturbanceSystem::BindFieldTexture does both: which
        /// variant of the water shader is in flight is not knowable here.
        static void PublishFieldTexture();

        /// The CPU read of the same field the shader samples, in the same
        /// addressing (absolute world XZ, bilinear). Returns the deep sentinel
        /// with a zero gradient wherever the field is unusable or the point is
        /// outside the window, which is exactly the disabled sample — so a
        /// caller need not test first.
        [[nodiscard]] static WaterShore::Sample SampleWorld(glm::vec2 worldXZ);

        // ---- the pure bake, exposed for tests ------------------------------

        /// Fill `outTexels` (kResolution^2, row-major, RGBA = depth, dDepth/dX,
        /// dDepth/dZ, 0) for the requested window. No GL, no ECS, no globals —
        /// this is the whole of what the field IS, and what WaterShoreWaveTest
        /// pins.
        static void BakeField(const WaterShoreBakeRequest& request,
                              std::span<const SeabedTerrain> terrains,
                              std::vector<glm::vec4>& outTexels);

        /// Bilinear read of a baked texel array, in the same addressing the
        /// GLSL side uses. Shared by SampleWorld and by the tests, so the CPU
        /// mirror cannot drift from the thing it mirrors.
        [[nodiscard]] static WaterShore::Sample SampleBaked(std::span<const glm::vec4> texels,
                                                            const WaterShoreBakeRequest& window,
                                                            glm::vec2 worldXZ);
    };
} // namespace OloEngine
