#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderConstants.h"
#include "OloEngine/Renderer/Shadow/ShadowAtlas.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace OloEngine
{
    class Shader;
    class EditorCamera;

    struct ShadowSettings
    {
        u32 Resolution = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);
        // Resolution of the local-light shadow atlas (issue #435). One square
        // DEPTH32F texture shared by every shadowed spot / point light; tiles
        // are allocated by priority (see ShadowAtlas.h). 4096² ≈ 64 MB.
        u32 AtlasResolution = 4096;
        f32 Bias = ShaderConstants::SHADOW_BIAS;
        f32 NormalBias = 0.01f;
        // With SoftShadows (PCSS) on, Softness is the light's apparent size — it
        // scales both the blocker-search region and the penumbra width, so larger
        // values give softer, more spread-out shadow edges. With PCSS off it
        // scales the legacy fixed PCF kernel. Range ~0.5..4 is sensible.
        f32 Softness = 1.0f;
        f32 MaxShadowDistance = 200.0f;
        f32 CascadeSplitLambda = 0.5f; // Practical split scheme blending factor
        bool Enabled = true;
        // Percentage-Closer Soft Shadows: contact-hardening variable-penumbra
        // filtering (sharp where the occluder touches the receiver, softening with
        // distance). Off falls back to the legacy fixed 3x3 hardware PCF.
        // Default OFF: a live A/B on SponzaCSM (RTX 4090, 1080p, 2026-07-02)
        // measured PCSS at ~43 ms of a 46.6 ms scene pass vs ~3 ms with PCF —
        // PCSS is an Ultra-tier opt-in (see QualityTiering), not the baseline.
        bool SoftShadows = false;
    };

    // @brief Manages shadow map textures, light-space matrices, and UBO uploads
    //
    // Owns the CSM Texture2DArray (4 cascades for the directional light) and
    // the budgeted local-light shadow ATLAS (issue #435): one large depth
    // texture whose square tiles hold every prioritised spot shadow and
    // point-light cube face. Entry selection/packing happens scene-side via
    // ShadowAtlas::Allocate; this class stores the resulting per-entry
    // light-space matrices + tile rects and uploads them to the ShadowData UBO.
    class ShadowMap
    {
      public:
        static constexpr u32 MAX_CSM_CASCADES = UBOStructures::ShadowUBO::MAX_CSM_CASCADES;
        static constexpr u32 MAX_SHADOW_ATLAS_ENTRIES = UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES;

        ShadowMap() = default;
        ~ShadowMap() = default;

        void Init(const ShadowSettings& settings = {});
        void Shutdown();

        // --- Per-frame operations ---

        // Compute CSM cascade splits and light-space matrices for a directional light
        void ComputeCSMCascades(
            const glm::vec3& lightDirection,
            const glm::mat4& cameraView,
            const glm::mat4& cameraProjection,
            f32 cameraNear,
            f32 cameraFar);

        // --- Shadow atlas entries (issue #435) ---

        // Light-space matrix builders. Pure; the scene composes these with the
        // ShadowAtlas allocation to fill entries.
        [[nodiscard]] static glm::mat4 BuildSpotLightMatrix(
            const glm::vec3& position,
            const glm::vec3& direction,
            f32 outerCutoffDegrees,
            f32 range);
        // 6 face VP matrices in +X,-X,+Y,-Y,+Z,-Z order (the face order the
        // shader's atlasCubeFace() helper selects by dominant axis).
        [[nodiscard]] static std::array<glm::mat4, 6> BuildPointLightFaceMatrices(
            const glm::vec3& position,
            f32 range);

        // One shadow-atlas CANDIDATE of the current frame, recorded by the Scene
        // shadow setup right after ShadowAtlas::Allocate ran (issue #607).
        //
        // ShadowAtlas::Allocate is a pure ranking + packing function whose inputs
        // (which light, what score) and losers (the casters that were starved)
        // evaporate the moment Scene's loop ends — only the winners' matrices and
        // tile rects survive, in the UBO. That makes a shadow-prioritisation bug
        // (a starved caster, a light packed at a 256px tile it needed 1024 for)
        // invisible in a screenshot AND unqueryable afterwards. Keeping the full
        // candidate list for the frame is what makes olo_shadow_atlas_layout able
        // to answer "why is this light's shadow missing / blocky?".
        //
        // Cleared by BeginFrame(), so it always describes the frame just set up.
        struct AtlasCasterRecord
        {
            u64 LightEntity = 0; // owning light entity's UUID (0 when unknown)
            ShadowAtlas::CasterType Type = ShadowAtlas::CasterType::Spot;
            // Which component produced it: "SpotLight" / "PointLight" / "SphereAreaLight".
            // Sphere-area lights compete as 6-face point casters, so Type alone
            // cannot distinguish them.
            const char* SourceKind = "SpotLight";
            f32 Score = 0.0f;       // ShadowAtlas::ComputeScore (0 = culled / not worth a tile)
            bool Allocated = false; // false = STARVED this frame (out of entries / lights / atlas space)
            u32 Rank = 0;           // priority rank among the ALLOCATED casters (0 = highest score); meaningless when starved
            u32 BaseEntry = 0;      // first atlas entry index (valid only when Allocated)
            u32 EntryCount = 0;     // 1 for spot, 6 for a point/sphere-area cube (valid only when Allocated)
        };

        // Replace this frame's candidate record (Scene calls it once, after the
        // allocation). Kept separate from SetAtlasEntry so the hot upload path is
        // untouched — this is diagnostics-only state.
        void SetAtlasLayout(std::vector<AtlasCasterRecord> layout)
        {
            m_AtlasLayout = std::move(layout);
        }
        [[nodiscard]] const std::vector<AtlasCasterRecord>& GetAtlasLayout() const
        {
            return m_AtlasLayout;
        }

        // Store one atlas entry (world-space light VP + pixel tile rect).
        void SetAtlasEntry(u32 entryIndex, const glm::mat4& lightVP, const ShadowAtlas::TileRect& rect);
        void SetAtlasEntryCount(u32 count)
        {
            m_UBOData.AtlasEntryCount = static_cast<i32>(std::min(count, MAX_SHADOW_ATLAS_ENTRIES));
        }

        [[nodiscard]] u32 GetAtlasEntryCount() const
        {
            return static_cast<u32>(m_UBOData.AtlasEntryCount);
        }
        // Out-of-range indices mirror SetAtlasEntry's silent tolerance: they
        // read a benign fallback (identity / zero-sized rect) instead of
        // indexing past the backing arrays.
        [[nodiscard]] const glm::mat4& GetAtlasEntryMatrix(u32 entryIndex) const
        {
            static const glm::mat4 s_Identity{ 1.0f };
            return (entryIndex < MAX_SHADOW_ATLAS_ENTRIES) ? m_AtlasEntryWorldMatrices[entryIndex] : s_Identity;
        }
        [[nodiscard]] const ShadowAtlas::TileRect& GetAtlasEntryRect(u32 entryIndex) const
        {
            static constexpr ShadowAtlas::TileRect s_Empty{};
            return (entryIndex < MAX_SHADOW_ATLAS_ENTRIES) ? m_AtlasEntryRects[entryIndex] : s_Empty;
        }

        // Upload all shadow data to the UBO. The light-space matrices are
        // stored world-space; they are shifted into camera-relative space
        // (issue #429) by `renderOrigin` on upload so the shadow sampling in
        // the lit pass matches the render-relative world positions. The shadow
        // *render* pass applies the same shift to the same matrices and to the
        // casters, so the two stay consistent. Pass
        // Renderer3D::GetRenderOrigin(); (0,0,0) is a no-op near origin.
        void UploadUBO(const glm::vec3& renderOrigin = glm::vec3(0.0f));

        // Bind the CSM texture array to the shadow texture slot
        void BindCSMTexture(u32 slot = ShaderBindingLayout::TEX_SHADOW) const;

        // Bind the local-light shadow atlas (1-layer depth array)
        void BindAtlasTexture(u32 slot = ShaderBindingLayout::TEX_SHADOW_ATLAS) const;

        // --- Accessors ---

        [[nodiscard]] const Ref<Texture2DArray>& GetCSMTextureArray() const
        {
            return m_CSMTextureArray;
        }
        [[nodiscard]] const Ref<Texture2DArray>& GetAtlasTextureArray() const
        {
            return m_AtlasTexture;
        }
        [[nodiscard]] u32 GetCSMRendererID() const;
        [[nodiscard]] u32 GetAtlasRendererID() const;

        // Render-graph resource names of the two raw-depth views below (issue
        // #607). They are pass-owned raw GL texture views, so until they are
        // declared under a stable name both olo_render_list_targets and
        // olo_render_capture_target are blind to them and the PCSS blocker
        // search's actual input cannot be inspected from an agent session.
        // Pinned here (not spelled at the declaration site) so the name the
        // pipeline declares and the name a tool asks for cannot drift.
        static constexpr const char* kCSMRawTargetName = "ShadowCSMRaw";
        static constexpr const char* kAtlasRawTargetName = "ShadowAtlasRaw";

        // Raw-depth (comparison-OFF) views aliasing the CSM / atlas depth
        // textures, bound as plain sampler2DArray so the PCSS blocker search
        // can read raw occluder depth (the hardware sampler2DArrayShadow only
        // yields the comparison result). Created alongside the textures in
        // Init(); 0 until then. See Renderer::CreateDepthArrayCompareOffView.
        [[nodiscard]] u32 GetCSMRawRendererID() const
        {
            return m_CSMRawViewID;
        }
        [[nodiscard]] u32 GetAtlasRawRendererID() const
        {
            return m_AtlasRawViewID;
        }

        // Placeholder shadow textures for when no real shadow map is available
        // this frame. Some drivers validate the bound texture target at draw
        // time when a sampler2DArrayShadow uniform exists, even if the shader
        // guards the actual sample with a uniform flag. These return 1x1
        // depth-comparison textures of the correct target. Lazy-initialised on
        // first call; freed in ShutdownPlaceholders(). Must be called from the
        // render thread. The atlas shares the CSM placeholders (same
        // sampler2DArrayShadow target).
        [[nodiscard]] static u32 GetCSMPlaceholderRendererID();
        [[nodiscard]] static u32 GetAtlasPlaceholderRendererID();
        // Comparison-OFF raw-depth placeholders (plain sampler2DArray) for the
        // PCSS raw-view slots when no real shadow map is bound this frame.
        [[nodiscard]] static u32 GetCSMRawPlaceholderRendererID();
        [[nodiscard]] static u32 GetAtlasRawPlaceholderRendererID();
        // Release placeholder textures. Called at renderer shutdown.
        static void ShutdownPlaceholders();

        [[nodiscard]] const glm::mat4& GetCSMMatrix(u32 cascade) const
        {
            return m_UBOData.DirectionalLightSpaceMatrices[cascade];
        }
        [[nodiscard]] const glm::vec4& GetCascadePlaneDistances() const
        {
            return m_UBOData.CascadePlaneDistances;
        }
        // True when at least one light requested shadows THIS frame — the
        // directional CSM (set by ComputeCSMCascades) or any atlas entry.
        // Populated during Scene shadow setup and reset by BeginFrame(), so it
        // is a per-frame signal, distinct from the global IsEnabled() toggle.
        // Used to skip the entire ShadowRenderPass (and its ×N cascade/entry
        // re-submission) when no light casts shadows — see issue #522.
        [[nodiscard]] bool AnyShadowsRequested() const
        {
            return m_UBOData.DirectionalShadowEnabled != 0 ||
                   m_UBOData.AtlasEntryCount > 0;
        }

        [[nodiscard]] u32 GetResolution() const
        {
            return m_Settings.Resolution;
        }
        [[nodiscard]] u32 GetAtlasResolution() const
        {
            return m_Settings.AtlasResolution;
        }
        [[nodiscard]] const ShadowSettings& GetSettings() const
        {
            return m_Settings;
        }
        void SetSettings(const ShadowSettings& settings);

        [[nodiscard]] bool IsEnabled() const
        {
            return m_Settings.Enabled;
        }
        void SetEnabled(bool enabled)
        {
            m_Settings.Enabled = enabled;
        }

        void SetDirectionalShadowEnabled(bool enabled)
        {
            m_UBOData.DirectionalShadowEnabled = enabled ? 1 : 0;
        }
        void SetCascadeDebugEnabled(bool enabled)
        {
            m_UBOData.CascadeDebugEnabled = enabled ? 1 : 0;
        }
        [[nodiscard]] bool IsCascadeDebugEnabled() const
        {
            return m_UBOData.CascadeDebugEnabled != 0;
        }

        // Reset per-frame state (call at BeginScene)
        void BeginFrame();

        // Accessors for shadow-pass rendering UBOs (shared across frames).
        // GetShadowModelUBO was retired alongside the renderer's ModelMatrixUBO;
        // shadow shaders now read transforms from the engine-wide InstanceBuffer
        // at binding 15 (see Renderer3D::GetModelInstanceBuffer()).
        [[nodiscard]] Ref<UniformBuffer>& GetShadowCameraUBO()
        {
            return m_ShadowCameraUBO;
        }
        [[nodiscard]] Ref<UniformBuffer>& GetShadowAnimationUBO()
        {
            return m_ShadowAnimationUBO;
        }

      private:
        // Compute frustum corners in world space for a given view-projection sub-frustum
        static std::array<glm::vec3, 8> GetFrustumCornersWorldSpace(
            const glm::mat4& projection,
            const glm::mat4& view);

        // Apply texel snapping to a light ortho matrix for stable CSM
        static glm::mat4 SnapToTexelGrid(
            const glm::mat4& lightProjection,
            const glm::mat4& lightView,
            u32 shadowMapResolution);

      private:
        ShadowSettings m_Settings;

        // Shadow map textures
        Ref<Texture2DArray> m_CSMTextureArray; // 4 layers for CSM cascades
        Ref<Texture2DArray> m_AtlasTexture;    // 1-layer local-light shadow atlas (issue #435)

        // Comparison-OFF raw-depth views of the two depth textures above (for
        // PCSS blocker search). Owned GL texture-view objects; deleted in Shutdown().
        u32 m_CSMRawViewID = 0;
        u32 m_AtlasRawViewID = 0;

        // World-space atlas entry state (UBO carries the camera-relative copies)
        std::array<glm::mat4, MAX_SHADOW_ATLAS_ENTRIES> m_AtlasEntryWorldMatrices{};
        std::array<ShadowAtlas::TileRect, MAX_SHADOW_ATLAS_ENTRIES> m_AtlasEntryRects{};

        // Diagnostics-only candidate list for the current frame (see
        // AtlasCasterRecord). Never read by the render path.
        std::vector<AtlasCasterRecord> m_AtlasLayout;

        // Shadow UBO
        Ref<UniformBuffer> m_ShadowUBO;
        UBOStructures::ShadowUBO m_UBOData{};

        // Temporary UBOs for shadow-pass rendering (reused each frame)
        Ref<UniformBuffer> m_ShadowCameraUBO;
        Ref<UniformBuffer> m_ShadowAnimationUBO;

        bool m_Initialized = false;
    };
} // namespace OloEngine
