#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderConstants.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <array>

namespace OloEngine
{
    class Shader;
    class EditorCamera;

    struct ShadowSettings
    {
        u32 Resolution = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);
        f32 Bias = ShaderConstants::SHADOW_BIAS;
        f32 NormalBias = 0.01f;
        f32 Softness = 1.0f;
        f32 MaxShadowDistance = 200.0f;
        f32 CascadeSplitLambda = 0.5f; // Practical split scheme blending factor
        bool Enabled = true;
    };

    // @brief Manages shadow map textures, light-space matrices, and UBO uploads
    //
    // Owns CSM Texture2DArray (4 cascades for directional light),
    // spot Texture2DArray (up to 4 spot lights), and depth cubemaps
    // (up to 4 point lights) for omnidirectional shadow mapping.
    class ShadowMap
    {
      public:
        static constexpr u32 MAX_CSM_CASCADES = UBOStructures::ShadowUBO::MAX_CSM_CASCADES;
        static constexpr u32 MAX_SPOT_SHADOWS = UBOStructures::ShadowUBO::MAX_SPOT_SHADOWS;
        static constexpr u32 MAX_POINT_SHADOWS = UBOStructures::ShadowUBO::MAX_POINT_SHADOWS;

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

        // Compute a single light-space matrix for a spot light
        void SetSpotLightShadow(
            u32 index,
            const glm::vec3& position,
            const glm::vec3& direction,
            f32 outerCutoff,
            f32 range);

        // Compute the 6 face VP matrices for a point light cubemap shadow
        void SetPointLightShadow(
            u32 index,
            const glm::vec3& position,
            f32 range);

        // Upload all shadow data to the UBO
        void UploadUBO();

        // Bind the CSM texture array to the shadow texture slot
        void BindCSMTexture(u32 slot = ShaderBindingLayout::TEX_SHADOW) const;

        // Bind the spot shadow texture array
        void BindSpotTexture(u32 slot = ShaderBindingLayout::TEX_SHADOW_SPOT) const;

        // Bind a point shadow cubemap
        void BindPointTexture(u32 index, u32 slot) const;

        // --- Accessors ---

        [[nodiscard]] const Ref<Texture2DArray>& GetCSMTextureArray() const
        {
            return m_CSMTextureArray;
        }
        [[nodiscard]] const Ref<Texture2DArray>& GetSpotTextureArray() const
        {
            return m_SpotTextureArray;
        }
        [[nodiscard]] u32 GetCSMRendererID() const;
        [[nodiscard]] u32 GetSpotRendererID() const;
        [[nodiscard]] u32 GetPointRendererID(u32 index) const;

        [[nodiscard]] const glm::mat4& GetCSMMatrix(u32 cascade) const
        {
            return m_UBOData.DirectionalLightSpaceMatrices[cascade];
        }
        [[nodiscard]] const glm::vec4& GetCascadePlaneDistances() const
        {
            return m_UBOData.CascadePlaneDistances;
        }
        [[nodiscard]] const glm::mat4& GetSpotMatrix(u32 index) const
        {
            return m_UBOData.SpotLightSpaceMatrices[index];
        }
        [[nodiscard]] i32 GetSpotShadowCount() const
        {
            return m_UBOData.SpotShadowCount;
        }
        [[nodiscard]] i32 GetPointShadowCount() const
        {
            return m_UBOData.PointShadowCount;
        }
        [[nodiscard]] const glm::mat4& GetPointFaceMatrix(u32 lightIndex, u32 face) const
        {
            return m_PointLightFaceMatrices[lightIndex][face];
        }
        [[nodiscard]] const glm::vec4& GetPointShadowParams(u32 index) const
        {
            return m_UBOData.PointLightShadowParams[index];
        }

        [[nodiscard]] u32 GetResolution() const
        {
            return m_Settings.Resolution;
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
        void SetSpotShadowCount(i32 count)
        {
            m_UBOData.SpotShadowCount = count;
        }
        void SetPointShadowCount(i32 count)
        {
            m_UBOData.PointShadowCount = count;
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

        // Accessors for shadow-pass rendering UBOs (shared across frames)
        [[nodiscard]] Ref<UniformBuffer>& GetShadowCameraUBO()
        {
            return m_ShadowCameraUBO;
        }
        [[nodiscard]] Ref<UniformBuffer>& GetShadowModelUBO()
        {
            return m_ShadowModelUBO;
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
        Ref<Texture2DArray> m_CSMTextureArray;                  // 4 layers for CSM cascades
        Ref<Texture2DArray> m_SpotTextureArray;                 // 4 layers for spot shadows
        std::array<u32, MAX_POINT_SHADOWS> m_PointCubemapIDs{}; // Depth cubemaps for point lights

        // Point light face VP matrices (6 per light)
        std::array<std::array<glm::mat4, 6>, MAX_POINT_SHADOWS> m_PointLightFaceMatrices{};

        // Shadow UBO
        Ref<UniformBuffer> m_ShadowUBO;
        UBOStructures::ShadowUBO m_UBOData{};

        // Temporary UBOs for shadow-pass rendering (reused each frame)
        Ref<UniformBuffer> m_ShadowCameraUBO;
        Ref<UniformBuffer> m_ShadowModelUBO;
        Ref<UniformBuffer> m_ShadowAnimationUBO;

        bool m_Initialized = false;
    };
} // namespace OloEngine
