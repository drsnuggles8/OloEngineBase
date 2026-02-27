#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Shader.h"

#include <functional>

namespace OloEngine
{
    // @brief Render pass for the main 3D scene.
    //
    // This pass handles the rendering of 3D scene objects to an offscreen framebuffer
    // using the command bucket system for efficient batching and sorting.
    //
    // DESIGN NOTE — PostExecuteCallback:
    // The engine's core rendering philosophy is "stateless layered command queue;
    // queue population separated from execution" (Molecular Matters style). All
    // standard meshes go through CommandBucket for sorting and batching.
    //
    // Terrain rendering bypasses the command bucket via PostExecuteCallback because:
    //   - It uses tessellation shaders (GL_PATCHES) not supported by the packet system
    //   - Per-chunk UBO updates (LOD tess factors) are inherently stateful
    //   - Streaming tile management requires dynamic draw calls
    // This is a deliberate, documented deviation. If the command system is extended
    // to support tessellation/patches, terrain should migrate back to it.
    class SceneRenderPass : public RenderPass
    {
      public:
        // Callback invoked after command bucket execution, while the scene framebuffer is still bound.
        // Used for terrain, decals, and other custom geometry that bypasses the command packet system.
        using PostExecuteCallback = std::function<void()>;

        SceneRenderPass();
        ~SceneRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetPostExecuteCallback(PostExecuteCallback callback)
        {
            m_PostExecuteCallback = std::move(callback);
        }

      private:
        PostExecuteCallback m_PostExecuteCallback;
    };
} // namespace OloEngine
