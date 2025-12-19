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

namespace OloEngine
{
    // @brief Render pass for the main 3D scene.
    //
    // This pass handles the rendering of 3D scene objects to an offscreen framebuffer
    // using the command bucket system for efficient batching and sorting.
    class SceneRenderPass : public RenderPass
    {
      public:
        SceneRenderPass();
        ~SceneRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
    };
} // namespace OloEngine
