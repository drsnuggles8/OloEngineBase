#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class FoliageRenderer;
    class Shader;

    // Indicates which shadow type is being rendered in the current callback invocation
    enum class ShadowPassType : u8
    {
        CSM,  // Directional light cascaded shadow map
        Spot, // Spot light 2D shadow map
        Point // Point light cubemap face (callback also receives light pos/far)
    };

    // POD shadow caster descriptors — collected during entity traversal, replayed per cascade/face.
    // This replaces the callback pattern: Scene.cpp adds casters during its entity loop,
    // and ShadowRenderPass::Execute() iterates them per light cascade/face with the
    // appropriate depth shader. No duplicate entity traversal, no per-frame lambda allocation.

    struct ShadowMeshCaster
    {
        RendererID vaoID = 0;
        u32 indexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
    };

    struct ShadowSkinnedCaster
    {
        RendererID vaoID = 0;
        u32 indexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
        u32 boneBufferOffset = 0;
        u32 boneCount = 0;
    };

    struct ShadowTerrainCaster
    {
        RendererID vaoID = 0;
        u32 indexCount = 0;
        u32 patchVertexCount = 3;
        glm::mat4 transform = glm::mat4(1.0f);
        RendererID heightmapTextureID = 0;
        ShaderBindingLayout::TerrainUBO terrainUBO{};
    };

    struct ShadowVoxelCaster
    {
        RendererID vaoID = 0;
        u32 indexCount = 0;
        glm::mat4 transform = glm::mat4(1.0f);
    };

    struct ShadowFoliageCaster
    {
        FoliageRenderer* renderer = nullptr;
        Ref<Shader> depthShader;
        f32 time = 0.0f;
    };

    // @brief Render pass for shadow map generation.
    //
    // Executes before SceneRenderPass. For each shadow-casting light,
    // renders scene geometry from the light's perspective into the
    // appropriate shadow map texture layer.
    //
    // Data-driven design: Scene.cpp adds shadow casters during its entity
    // traversal loop. Execute() iterates the caster lists per cascade/face,
    // binding the appropriate depth shader for each geometry type.
    // No callbacks, no duplicate entity traversal.
    class ShadowRenderPass : public RenderPass
    {
      public:
        ShadowRenderPass();
        ~ShadowRenderPass() override;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetShadowMap(ShadowMap* shadowMap)
        {
            m_ShadowMap = shadowMap;
        }

        // Shadow caster submission — called during Scene entity traversal
        void AddMeshCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform);
        void AddSkinnedCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform,
                              u32 boneBufferOffset, u32 boneCount);
        void AddTerrainCaster(RendererID vaoID, u32 indexCount, u32 patchVertexCount,
                              const glm::mat4& transform, RendererID heightmapTextureID,
                              const ShaderBindingLayout::TerrainUBO& terrainUBO);
        void AddVoxelCaster(RendererID vaoID, u32 indexCount, const glm::mat4& transform);
        void AddFoliageCaster(FoliageRenderer* renderer, const Ref<Shader>& depthShader, f32 time);

      private:
        void RenderCascadeOrFace(const glm::mat4& lightVP, ShadowPassType type, u32 layerOrLight);

        ShadowMap* m_ShadowMap = nullptr;
        Ref<Framebuffer> m_ShadowFramebuffer; // Depth-only FBO for shadow rendering

        // Shadow caster lists — cleared after each Execute()
        std::vector<ShadowMeshCaster> m_MeshCasters;
        std::vector<ShadowSkinnedCaster> m_SkinnedCasters;
        std::vector<ShadowTerrainCaster> m_TerrainCasters;
        std::vector<ShadowVoxelCaster> m_VoxelCasters;
        std::vector<ShadowFoliageCaster> m_FoliageCasters;

        bool m_WarnedOnce = false;
        bool m_LoggedOnce = false;
    };
} // namespace OloEngine
