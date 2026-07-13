#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace OloEngine
{
    void ShadowMap::Init(const ShadowSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        m_Settings = settings;

        // Create CSM texture array (4 cascades, depth-only, hardware comparison)
        Texture2DArraySpecification csmSpec;
        csmSpec.Width = m_Settings.Resolution;
        csmSpec.Height = m_Settings.Resolution;
        csmSpec.Layers = MAX_CSM_CASCADES;
        csmSpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
        csmSpec.DepthComparisonMode = true;
        m_CSMTextureArray = Texture2DArray::Create(csmSpec);

        // Create the local-light shadow atlas (issue #435): one large 1-layer
        // depth array holding every prioritised spot shadow / point-light cube
        // face as a square sub-tile. A 1-layer ARRAY (not a plain 2D texture)
        // so it shares the sampler2DArrayShadow sampling helpers, the
        // AttachDepthTextureArrayLayer render path, and the CSM placeholders.
        Texture2DArraySpecification atlasSpec;
        atlasSpec.Width = m_Settings.AtlasResolution;
        atlasSpec.Height = m_Settings.AtlasResolution;
        atlasSpec.Layers = 1;
        atlasSpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
        atlasSpec.DepthComparisonMode = true;
        m_AtlasTexture = Texture2DArray::Create(atlasSpec);

        // Comparison-OFF raw-depth views aliasing the CSM / atlas textures,
        // used by the PCSS blocker search (the hardware comparison sampler
        // can't read raw occluder depth). These alias the same immutable
        // storage, so the sampler2DArrayShadow bindings are unaffected.
        m_CSMRawViewID = RenderCommand::CreateDepthArrayCompareOffView(
            m_CSMTextureArray->GetRendererID(), MAX_CSM_CASCADES);
        m_AtlasRawViewID = RenderCommand::CreateDepthArrayCompareOffView(
            m_AtlasTexture->GetRendererID(), 1);

        // Create shadow UBO at binding 6
        m_ShadowUBO = UniformBuffer::Create(
            UBOStructures::ShadowUBO::GetSize(),
            ShaderBindingLayout::UBO_SHADOW);

        // Create temporary UBOs for the shadow rendering pass. m_ShadowModelUBO
        // was retired alongside Renderer3D::ModelMatrixUBO — shadow shaders
        // now read transforms from the engine-wide InstanceBuffer SSBO at
        // binding 15.
        m_ShadowCameraUBO = UniformBuffer::Create(
            ShaderBindingLayout::CameraUBO::GetSize(),
            ShaderBindingLayout::UBO_CAMERA);
        m_ShadowAnimationUBO = UniformBuffer::Create(
            ShaderBindingLayout::AnimationUBO::GetSize(),
            ShaderBindingLayout::UBO_ANIMATION);

        // Initialize UBO data to defaults
        m_UBOData = {};
        m_UBOData.ShadowParams = glm::vec4(
            m_Settings.Bias,
            m_Settings.NormalBias,
            m_Settings.Softness,
            m_Settings.MaxShadowDistance);
        m_UBOData.ShadowMapResolution = static_cast<i32>(m_Settings.Resolution);
        m_UBOData.AtlasResolution = static_cast<i32>(m_Settings.AtlasResolution);
        m_UBOData.SoftShadowMode = m_Settings.SoftShadows ? 1 : 0;

        m_Initialized = true;
        OLO_CORE_INFO("ShadowMap initialized: {}x{} CSM resolution ({} cascades), {}x{} shadow atlas ({} entry budget)",
                      m_Settings.Resolution, m_Settings.Resolution, MAX_CSM_CASCADES,
                      m_Settings.AtlasResolution, m_Settings.AtlasResolution, MAX_SHADOW_ATLAS_ENTRIES);
    }

    void ShadowMap::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (m_CSMRawViewID != 0)
        {
            RenderCommand::DeleteTexture(m_CSMRawViewID);
            m_CSMRawViewID = 0;
        }
        if (m_AtlasRawViewID != 0)
        {
            RenderCommand::DeleteTexture(m_AtlasRawViewID);
            m_AtlasRawViewID = 0;
        }

        m_CSMTextureArray.Reset();
        m_AtlasTexture.Reset();

        m_ShadowUBO.Reset();
        m_ShadowCameraUBO.Reset();
        m_ShadowAnimationUBO.Reset();
        m_Initialized = false;
    }

    void ShadowMap::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();

        // Reset per-frame state
        m_UBOData.DirectionalShadowEnabled = 0;
        m_UBOData.AtlasEntryCount = 0;
        // The diagnostics candidate list describes ONE frame's allocation
        // (issue #607); a frame with no shadow candidates must report an empty
        // layout, not last frame's winners.
        m_AtlasLayout.clear();
    }

    void ShadowMap::ComputeCSMCascades(
        const glm::vec3& lightDirection,
        const glm::mat4& cameraView,
        const glm::mat4& cameraProjection,
        f32 cameraNear,
        f32 cameraFar)
    {
        OLO_PROFILE_FUNCTION();

        // Clamp far plane to max shadow distance
        const f32 effectiveFar = std::min(cameraFar, m_Settings.MaxShadowDistance);
        const f32 lambda = m_Settings.CascadeSplitLambda;

        // Compute cascade split distances using practical split scheme
        // Blends between logarithmic and uniform distribution
        std::array<f32, MAX_CSM_CASCADES + 1> cascadeSplits{};
        cascadeSplits[0] = cameraNear;

        for (u32 i = 1; i <= MAX_CSM_CASCADES; ++i)
        {
            const f32 p = static_cast<f32>(i) / static_cast<f32>(MAX_CSM_CASCADES);
            const f32 logSplit = cameraNear * std::pow(effectiveFar / cameraNear, p);
            const f32 uniformSplit = cameraNear + (effectiveFar - cameraNear) * p;
            cascadeSplits[i] = std::lerp(uniformSplit, logSplit, lambda);
        }

        // Store cascade far planes (view-space distances) for shader
        m_UBOData.CascadePlaneDistances = glm::vec4(
            cascadeSplits[1], cascadeSplits[2],
            cascadeSplits[3], cascadeSplits[4]);

        // For each cascade, compute the light-space VP matrix
        const glm::vec3 lightDir = glm::normalize(lightDirection);

        // Get full frustum corners once (shared across all cascades)
        const auto fullCorners = GetFrustumCornersWorldSpace(cameraProjection, cameraView);
        const f32 fullRange = cameraFar - cameraNear;

        // Extract camera world position from the inverse view matrix.
        // The cascade box is positioned RELATIVE to this point, NOT relative to
        // the rotated frustum center — that's the key to camera-rotation-stable
        // shadows. Centering on the rotated frustum-corner-average makes the
        // cascade orbit the camera as the camera rotates, which causes visible
        // shadow drift even with texel snapping (the box translates smoothly,
        // snapping only fixes sub-texel jitter).
        const glm::mat4 invView = glm::inverse(cameraView);
        const glm::vec3 cameraWorldPos = glm::vec3(invView[3]);

        for (u32 cascade = 0; cascade < MAX_CSM_CASCADES; ++cascade)
        {
            const f32 nearSplit = cascadeSplits[cascade];
            const f32 farSplit = cascadeSplits[cascade + 1];

            // Interpolate full-frustum corners to get the sub-frustum for this cascade
            // 0-3: near plane corners, 4-7: far plane corners
            std::array<glm::vec3, 8> subCorners{};
            const f32 nearT = (nearSplit - cameraNear) / fullRange;
            const f32 farT = (farSplit - cameraNear) / fullRange;

            for (u32 i = 0; i < 4; ++i)
            {
                const glm::vec3 cornerRay = fullCorners[i + 4] - fullCorners[i];
                subCorners[i] = fullCorners[i] + cornerRay * nearT;
                subCorners[i + 4] = fullCorners[i] + cornerRay * farT;
            }

            // Cascade center pinned to camera POSITION. Camera rotation does
            // not move this point, so the X/Y ortho footprint is stationary in
            // world space as the user pans/turns the view — combined with the
            // texel snapping below, shadow texels stay locked to world geometry.
            // The cost is that the cascade now covers a sphere around the
            // camera rather than just the visible frustum slice, so a portion
            // of the shadow-map resolution falls outside the view; for typical
            // FOVs (60-90°) the resolution loss is ~30-40% per cascade, well
            // worth the gain in visual stability.
            const glm::vec3 center = cameraWorldPos;

            // Radius = farthest-corner distance from cameraWorldPos. Because
            // a perspective frustum is camera-anchored, the distance from the
            // camera to a frustum corner depends only on FOV/aspect/depth —
            // all orientation-invariant — so this value is stable across rotation.
            f32 radius = 0.0f;
            for (const auto& corner : subCorners)
            {
                radius = std::max(radius, glm::length(corner - center));
            }

            // Round up to texel boundary so the sphere maps to whole texels
            const f32 texelsPerUnit = static_cast<f32>(m_Settings.Resolution) / (radius * 2.0f);
            radius = std::ceil(radius * texelsPerUnit) / texelsPerUnit;

            // Place the light eye FAR away along the light direction so every
            // potential shadow caster — including tall columns that extend high
            // above the camera frustum — is in front of the eye, not behind.
            // Using just `radius` as the eye distance (as the original code did)
            // would put the eye INSIDE the cascade volume, clipping anything
            // taller than `radius` away in the sun direction. The fixed large
            // distance below dwarfs any practical scene scale.
            constexpr f32 kLightEyeDistance = 1000.0f;
            const glm::mat4 lightView = glm::lookAt(
                center - lightDir * kLightEyeDistance,
                center,
                glm::vec3(0.0f, 1.0f, 0.0f));

            // Compute Z bounds in light space from the sub-frustum corners.
            // In view space, forward is -Z, so corners in front of the eye have negative z.
            f32 minZ = std::numeric_limits<f32>::max();
            f32 maxZ = std::numeric_limits<f32>::lowest();
            for (const auto& corner : subCorners)
            {
                const f32 z = (lightView * glm::vec4(corner, 1.0f)).z;
                minZ = std::min(minZ, z);
                maxZ = std::max(maxZ, z);
            }

            // Convert to positive distances from the eye and add generous padding
            // so shadow casters OUTSIDE the camera frustum (tall walls/columns/
            // ceilings above the viewer) still register in the depth map. The
            // padding must comfortably exceed the tallest caster in the scene.
            constexpr f32 zPadding = 200.0f;
            const f32 nearDist = std::max(-maxZ - zPadding, 0.1f);
            const f32 farDist = -minZ + zPadding;

            // Stable ortho: sphere radius for X/Y, computed range for Z
            glm::mat4 lightProjection = glm::ortho(
                -radius, radius,
                -radius, radius,
                nearDist, farDist);

            // Apply texel snapping to stabilize the shadow map
            const glm::mat4 snappedProjection = SnapToTexelGrid(
                lightProjection, lightView, m_Settings.Resolution);

            m_UBOData.DirectionalLightSpaceMatrices[cascade] = snappedProjection * lightView;
        }

        m_UBOData.DirectionalShadowEnabled = 1;
    }

    glm::mat4 ShadowMap::BuildSpotLightMatrix(
        const glm::vec3& position,
        const glm::vec3& direction,
        f32 outerCutoffDegrees,
        f32 range)
    {
        OLO_PROFILE_FUNCTION();

        const glm::vec3 dir = glm::normalize(direction);

        // Compute up vector (avoid parallel to direction)
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(dir, up)) > 0.99f)
        {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        const glm::mat4 lightView = glm::lookAt(position, position + dir, up);

        // outerCutoff is in degrees, convert to FOV for perspective projection
        const f32 fov = glm::radians(outerCutoffDegrees * 2.0f);
        const glm::mat4 lightProjection = glm::perspective(fov, 1.0f, 0.1f, range);

        return lightProjection * lightView;
    }

    std::array<glm::mat4, 6> ShadowMap::BuildPointLightFaceMatrices(
        const glm::vec3& position,
        f32 range)
    {
        OLO_PROFILE_FUNCTION();

        // Standard 90°-FOV cube faces, rendered PROJECTIVELY into atlas tiles
        // (standard depth compare via the entry matrix — no more linear-
        // distance cubemap depth). Face order matches the shader's dominant-
        // axis selector atlasCubeFace(): +X,-X,+Y,-Y,+Z,-Z.
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, range);

        return {
            proj * glm::lookAt(position, position + glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)),  // +X
            proj * glm::lookAt(position, position + glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)), // -X
            proj * glm::lookAt(position, position + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)),   // +Y
            proj * glm::lookAt(position, position + glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)), // -Y
            proj * glm::lookAt(position, position + glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)),  // +Z
            proj * glm::lookAt(position, position + glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)), // -Z
        };
    }

    void ShadowMap::SetAtlasEntry(u32 entryIndex, const glm::mat4& lightVP, const ShadowAtlas::TileRect& rect)
    {
        if (entryIndex >= MAX_SHADOW_ATLAS_ENTRIES)
        {
            return;
        }

        m_AtlasEntryWorldMatrices[entryIndex] = lightVP;
        m_AtlasEntryRects[entryIndex] = rect;
        m_UBOData.AtlasEntryScaleOffset[entryIndex] =
            ShadowAtlas::TileScaleOffset(rect, m_Settings.AtlasResolution);
    }

    void ShadowMap::UploadUBO(const glm::vec3& renderOrigin)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_ShadowUBO)
        {
            return;
        }

        // Update shadow params from current settings
        auto data = m_UBOData;
        data.ShadowParams = glm::vec4(
            m_Settings.Bias,
            m_Settings.NormalBias,
            m_Settings.Softness,
            m_Settings.MaxShadowDistance);
        data.ShadowMapResolution = static_cast<i32>(m_Settings.Resolution);
        data.AtlasResolution = static_cast<i32>(m_Settings.AtlasResolution);
        data.SoftShadowMode = m_Settings.SoftShadows ? 1 : 0;

        // Camera-relative (issue #429): the lit pass samples shadows using the
        // fragment's render-relative world position, so the light-space matrices
        // must map relative world -> light clip (multiply by translate(origin)).
        // m_UBOData/m_AtlasEntryWorldMatrices stay world-space; the shadow render
        // pass applies the identical shift to the same matrices + casters,
        // keeping rendered depth and sampled depth in the same space. No-op near
        // origin.
        for (u32 c = 0; c < MAX_CSM_CASCADES; ++c)
            data.DirectionalLightSpaceMatrices[c] = MakeViewProjectionRelative(m_UBOData.DirectionalLightSpaceMatrices[c], renderOrigin);
        const u32 entryCount = std::min(static_cast<u32>(m_UBOData.AtlasEntryCount), MAX_SHADOW_ATLAS_ENTRIES);
        for (u32 e = 0; e < entryCount; ++e)
            data.AtlasEntryMatrices[e] = MakeViewProjectionRelative(m_AtlasEntryWorldMatrices[e], renderOrigin);

        m_ShadowUBO->SetData(&data, UBOStructures::ShadowUBO::GetSize());
        // Re-establish binding point 6 every frame to guard against
        // anything (init ordering, driver quirks) that might unbind it.
        m_ShadowUBO->Bind();
    }

    void ShadowMap::BindCSMTexture(u32 slot) const
    {
        if (m_CSMTextureArray)
        {
            m_CSMTextureArray->Bind(slot);
        }
    }

    void ShadowMap::BindAtlasTexture(u32 slot) const
    {
        if (m_AtlasTexture)
        {
            m_AtlasTexture->Bind(slot);
        }
    }

    u32 ShadowMap::GetCSMRendererID() const
    {
        return m_CSMTextureArray ? m_CSMTextureArray->GetRendererID() : 0;
    }

    u32 ShadowMap::GetAtlasRendererID() const
    {
        return m_AtlasTexture ? m_AtlasTexture->GetRendererID() : 0;
    }

    // ------------------------------------------------------------------
    // Placeholder shadow textures
    // ------------------------------------------------------------------
    // Owned at namespace scope so they survive across multiple ShadowMap
    // instances (e.g. resolution changes Shutdown+Init the per-instance
    // textures but the placeholders only depend on type, not size).
    namespace
    {
        Ref<Texture2DArray> g_PlaceholderShadowArray;
        u32 g_PlaceholderShadowArrayRaw = 0u; // compare-OFF view of the array above

        Ref<Texture2DArray> CreatePlaceholderShadowArray()
        {
            Texture2DArraySpecification spec;
            spec.Width = 1u;
            spec.Height = 1u;
            spec.Layers = 1u;
            spec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
            spec.DepthComparisonMode = true;
            return Texture2DArray::Create(spec);
        }
    } // namespace

    u32 ShadowMap::GetCSMPlaceholderRendererID()
    {
        if (!g_PlaceholderShadowArray)
            g_PlaceholderShadowArray = CreatePlaceholderShadowArray();
        return g_PlaceholderShadowArray ? g_PlaceholderShadowArray->GetRendererID() : 0u;
    }

    u32 ShadowMap::GetAtlasPlaceholderRendererID()
    {
        // The atlas uses the same sampler2DArrayShadow type as CSM — share.
        return GetCSMPlaceholderRendererID();
    }

    u32 ShadowMap::GetCSMRawPlaceholderRendererID()
    {
        if (g_PlaceholderShadowArrayRaw == 0u)
        {
            const u32 src = GetCSMPlaceholderRendererID(); // ensures the array exists
            if (src != 0u)
                g_PlaceholderShadowArrayRaw = RenderCommand::CreateDepthArrayCompareOffView(src, 1u);
        }
        return g_PlaceholderShadowArrayRaw;
    }

    u32 ShadowMap::GetAtlasRawPlaceholderRendererID()
    {
        // Same plain sampler2DArray placeholder as CSM raw — share.
        return GetCSMRawPlaceholderRendererID();
    }

    void ShadowMap::ShutdownPlaceholders()
    {
        if (g_PlaceholderShadowArrayRaw != 0u)
        {
            RenderCommand::DeleteTexture(g_PlaceholderShadowArrayRaw);
            g_PlaceholderShadowArrayRaw = 0u;
        }
        g_PlaceholderShadowArray.Reset();
    }

    void ShadowMap::SetSettings(const ShadowSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        const bool resolutionChanged = settings.Resolution != m_Settings.Resolution ||
                                       settings.AtlasResolution != m_Settings.AtlasResolution;
        m_Settings = settings;

        if (resolutionChanged && m_Initialized)
        {
            // Recreate textures at new resolution
            Shutdown();
            Init(m_Settings);
        }
    }

    std::array<glm::vec3, 8> ShadowMap::GetFrustumCornersWorldSpace(
        const glm::mat4& projection,
        const glm::mat4& view)
    {
        OLO_PROFILE_FUNCTION();

        const glm::mat4 invVP = glm::inverse(projection * view);

        // NDC corners of the full frustum [-1,1]^3
        std::array<glm::vec3, 8> corners{};
        u32 idx = 0;
        for (i32 z = 0; z <= 1; ++z)
        {
            for (i32 y = 0; y <= 1; ++y)
            {
                for (i32 x = 0; x <= 1; ++x)
                {
                    const glm::vec4 ndc(
                        2.0f * static_cast<f32>(x) - 1.0f,
                        2.0f * static_cast<f32>(y) - 1.0f,
                        2.0f * static_cast<f32>(z) - 1.0f,
                        1.0f);
                    glm::vec4 world = invVP * ndc;
                    corners[idx++] = glm::vec3(world) / world.w;
                }
            }
        }

        return corners;
    }

    glm::mat4 ShadowMap::SnapToTexelGrid(
        const glm::mat4& lightProjection,
        const glm::mat4& lightView,
        u32 shadowMapResolution)
    {
        OLO_PROFILE_FUNCTION();

        // Compute the size of a shadow map texel in world space
        const glm::mat4 shadowMatrix = lightProjection * lightView;
        glm::vec4 shadowOrigin = shadowMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        shadowOrigin *= static_cast<f32>(shadowMapResolution) / 2.0f;

        const glm::vec4 roundedOrigin = glm::round(shadowOrigin);
        glm::vec4 roundOffset = roundedOrigin - shadowOrigin;
        roundOffset *= 2.0f / static_cast<f32>(shadowMapResolution);
        roundOffset.z = 0.0f;
        roundOffset.w = 0.0f;

        glm::mat4 snapped = lightProjection;
        snapped[3] += roundOffset;
        return snapped;
    }
} // namespace OloEngine
