#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>
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

        // Create spot shadow texture array (4 spot lights, depth-only, hardware comparison)
        Texture2DArraySpecification spotSpec;
        spotSpec.Width = m_Settings.Resolution;
        spotSpec.Height = m_Settings.Resolution;
        spotSpec.Layers = MAX_SPOT_SHADOWS;
        spotSpec.Format = Texture2DArrayFormat::DEPTH_COMPONENT32F;
        spotSpec.DepthComparisonMode = true;
        m_SpotTextureArray = Texture2DArray::Create(spotSpec);

        // Create point shadow depth cubemaps (one per point light)
        for (u32 i = 0; i < MAX_POINT_SHADOWS; ++i)
        {
            glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_PointCubemapIDs[i]);
            glTextureStorage2D(m_PointCubemapIDs[i], 1, GL_DEPTH_COMPONENT32F,
                               static_cast<GLsizei>(m_Settings.Resolution),
                               static_cast<GLsizei>(m_Settings.Resolution));

            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

            // Enable hardware depth comparison for samplerCubeShadow
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTextureParameteri(m_PointCubemapIDs[i], GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }

        // Create shadow UBO at binding 6
        m_ShadowUBO = UniformBuffer::Create(
            UBOStructures::ShadowUBO::GetSize(),
            ShaderBindingLayout::UBO_SHADOW);

        // Create temporary UBOs for the shadow rendering pass
        m_ShadowCameraUBO = UniformBuffer::Create(
            ShaderBindingLayout::CameraUBO::GetSize(),
            ShaderBindingLayout::UBO_CAMERA);
        m_ShadowModelUBO = UniformBuffer::Create(
            ShaderBindingLayout::ModelUBO::GetSize(),
            ShaderBindingLayout::UBO_MODEL);
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

        m_Initialized = true;
        OLO_CORE_INFO("ShadowMap initialized: {}x{} resolution, {} CSM cascades, {} point cubemaps",
                      m_Settings.Resolution, m_Settings.Resolution, MAX_CSM_CASCADES, MAX_POINT_SHADOWS);
    }

    void ShadowMap::Shutdown()
    {
        m_CSMTextureArray.Reset();
        m_SpotTextureArray.Reset();

        for (u32 i = 0; i < MAX_POINT_SHADOWS; ++i)
        {
            if (m_PointCubemapIDs[i] != 0)
            {
                glDeleteTextures(1, &m_PointCubemapIDs[i]);
                m_PointCubemapIDs[i] = 0;
            }
        }

        m_ShadowUBO.Reset();
        m_ShadowCameraUBO.Reset();
        m_ShadowModelUBO.Reset();
        m_ShadowAnimationUBO.Reset();
        m_Initialized = false;
    }

    void ShadowMap::BeginFrame()
    {
        // Reset per-frame state
        m_UBOData.DirectionalShadowEnabled = 0;
        m_UBOData.SpotShadowCount = 0;
        m_UBOData.PointShadowCount = 0;
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
            cascadeSplits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
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

            // Compute the center of the sub-frustum
            glm::vec3 center(0.0f);
            for (const auto& corner : subCorners)
            {
                center += corner;
            }
            center /= 8.0f;

            // Compute bounding sphere radius for stable X/Y coverage.
            // A sphere's radius stays constant regardless of camera rotation,
            // preventing shadow coverage from shifting every frame.
            f32 radius = 0.0f;
            for (const auto& corner : subCorners)
            {
                radius = std::max(radius, glm::length(corner - center));
            }

            // Round up to texel boundary so the sphere maps to whole texels
            const f32 texelsPerUnit = static_cast<f32>(m_Settings.Resolution) / (radius * 2.0f);
            radius = std::ceil(radius * texelsPerUnit) / texelsPerUnit;

            // Build the light view matrix looking at the center
            const glm::mat4 lightView = glm::lookAt(
                center - lightDir * radius,
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

            // Convert to positive distances from the eye and add padding
            // for shadow casters outside the camera frustum.
            // -maxZ = closest distance, -minZ = farthest distance (both positive).
            constexpr f32 zPadding = 50.0f;
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

    void ShadowMap::SetSpotLightShadow(
        u32 index,
        const glm::vec3& position,
        const glm::vec3& direction,
        f32 outerCutoff,
        f32 range)
    {
        if (index >= MAX_SPOT_SHADOWS)
        {
            return;
        }

        const glm::vec3 dir = glm::normalize(direction);

        // Compute up vector (avoid parallel to direction)
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(dir, up)) > 0.99f)
        {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        const glm::mat4 lightView = glm::lookAt(position, position + dir, up);

        // outerCutoff is in degrees, convert to FOV for perspective projection
        const f32 fov = glm::radians(outerCutoff * 2.0f);
        const glm::mat4 lightProjection = glm::perspective(fov, 1.0f, 0.1f, range);

        m_UBOData.SpotLightSpaceMatrices[index] = lightProjection * lightView;
    }

    void ShadowMap::SetPointLightShadow(
        u32 index,
        const glm::vec3& position,
        f32 range)
    {
        if (index >= MAX_POINT_SHADOWS)
        {
            return;
        }

        // Store position and far plane for shader linear depth comparison
        m_UBOData.PointLightShadowParams[index] = glm::vec4(position, range);

        // Build 6 face VP matrices for cubemap rendering
        const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, range);

        m_PointLightFaceMatrices[index][0] = proj * glm::lookAt(position, position + glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));  // +X
        m_PointLightFaceMatrices[index][1] = proj * glm::lookAt(position, position + glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)); // -X
        m_PointLightFaceMatrices[index][2] = proj * glm::lookAt(position, position + glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));   // +Y
        m_PointLightFaceMatrices[index][3] = proj * glm::lookAt(position, position + glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)); // -Y
        m_PointLightFaceMatrices[index][4] = proj * glm::lookAt(position, position + glm::vec3(0, 0, 1), glm::vec3(0, -1, 0));  // +Z
        m_PointLightFaceMatrices[index][5] = proj * glm::lookAt(position, position + glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)); // -Z
    }

    void ShadowMap::UploadUBO()
    {
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

    void ShadowMap::BindSpotTexture(u32 slot) const
    {
        if (m_SpotTextureArray)
        {
            m_SpotTextureArray->Bind(slot);
        }
    }

    void ShadowMap::BindPointTexture(u32 index, u32 slot) const
    {
        if (index < MAX_POINT_SHADOWS && m_PointCubemapIDs[index] != 0)
        {
            glBindTextureUnit(slot, m_PointCubemapIDs[index]);
        }
    }

    u32 ShadowMap::GetCSMRendererID() const
    {
        return m_CSMTextureArray ? m_CSMTextureArray->GetRendererID() : 0;
    }

    u32 ShadowMap::GetSpotRendererID() const
    {
        return m_SpotTextureArray ? m_SpotTextureArray->GetRendererID() : 0;
    }

    u32 ShadowMap::GetPointRendererID(u32 index) const
    {
        return (index < MAX_POINT_SHADOWS) ? m_PointCubemapIDs[index] : 0;
    }

    void ShadowMap::SetSettings(const ShadowSettings& settings)
    {
        const bool resolutionChanged = settings.Resolution != m_Settings.Resolution;
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
