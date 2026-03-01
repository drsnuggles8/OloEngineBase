#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    class ComputeShader;
    class Texture2D;
    class UniformBuffer;

    /**
     * @brief Camera-following snow accumulation & deformation system.
     *
     * Maintains an R32F snow-depth texture (clipmap) that is updated each
     * frame by compute shaders:
     *   - Snow_Accumulate.comp — additive snow growth, melt, restoration
     *   - Snow_Deform.comp    — stamp-based deformation from tagged entities
     *   - Snow_Clear.comp     — zeroes the depth buffer on scene load
     *
     * The depth texture is sampled by:
     *   - Terrain TES       — vertex displacement
     *   - Terrain FS / PBR  — snow coverage weight boost
     *
     * Follows the same static-singleton pattern as WindSystem.
     */
    class SnowAccumulationSystem
    {
      public:
        /// Initialize GPU resources (compute shaders, R32F texture, UBO).
        static void Init();

        /// Release GPU resources.
        static void Shutdown();

        /// @return true after Init() succeeds.
        [[nodiscard]] static bool IsInitialized();

        /**
         * @brief Run accumulation for this frame.
         *
         * Recenters the clipmap on the camera, dispatches Snow_Accumulate
         * compute, and uploads SnowAccumulationUBOData so terrain/PBR
         * shaders can sample. Deformation (Snow_Deform) is dispatched
         * separately via SubmitDeformers().
         *
         * @param settings   Current scene-level snow accumulation parameters.
         * @param cameraPos  Camera world position (clipmap follows this).
         * @param dt         Frame delta time.
         */
        static void Update(const SnowAccumulationSettings& settings,
                           const glm::vec3& cameraPos,
                           Timestep dt);

        /**
         * @brief Submit deformer stamps for the current frame.
         *
         * Uploads an array of stamp descriptors to the SSBO and dispatches
         * the Snow_Deform compute shader.
         *
         * @param stamps  Packed stamp data: { vec4(posX, posY, posZ, radius), vec4(depth, falloff, compaction, 0) } per stamp.
         * @param count   Number of stamps (each stamp = 2 × vec4 = 32 bytes).
         */
        static void SubmitDeformers(const glm::vec4* stamps, u32 count);

        /// Bind the snow depth texture to TEX_SNOW_DEPTH (slot 30).
        static void BindSnowDepthTexture();

        /// @return OpenGL texture ID of the snow depth map (for debug overlay).
        [[nodiscard]] static u32 GetSnowDepthTextureID();

        /// Mark the snow depth buffer for clearing; the actual zeroing
        /// occurs during the next Update() pass (via Snow_Clear dispatch).
        static void Reset();

      private:
        struct SnowAccumulationData
        {
            Ref<ComputeShader> m_AccumulateShader;
            Ref<ComputeShader> m_DeformShader;
            Ref<ComputeShader> m_ClearShader;
            Ref<Texture2D> m_SnowDepthTexture;    // R32F, 2048×2048
            Ref<UniformBuffer> m_AccumulationUBO; // binding 16
            Ref<StorageBuffer> m_DeformerSSBO;    // SSBO for deformer stamps (binding 7)
            SnowAccumulationUBOData m_GPUData;

            glm::vec3 m_PrevClipmapCenter = glm::vec3(0.0f);
            u32 m_TextureResolution = 0; // Allocated texture size (authoritative)
            f32 m_AccumulatedTime = 0.0f;
            bool m_Initialized = false;
            bool m_NeedsClear = true;
        };

        static void ComputeClipmapMatrices(const glm::vec3& center,
                                           const SnowAccumulationSettings& settings);

        static SnowAccumulationData s_Data;
    };
} // namespace OloEngine
