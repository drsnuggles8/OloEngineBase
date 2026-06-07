#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkyCubemapBake.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine::SkyBake
{
    namespace
    {
        // The six look directions + 90-degree projection used to capture each
        // cubemap face. Same convention as IBLPrecompute::RenderToCubemap so the
        // baked cubemap is sampled consistently by the rest of the IBL path.
        struct CaptureMatrices
        {
            glm::mat4 Views[6];
            glm::mat4 Projection;
        };

        const CaptureMatrices& GetCaptureMatrices()
        {
            static const CaptureMatrices kMatrices = []
            {
                CaptureMatrices m{};
                m.Views[0] = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[1] = glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[2] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                m.Views[3] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
                m.Views[4] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[5] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
                return m;
            }();
            return kMatrices;
        }
    } // namespace

    bool RenderSkyToCubemap(Ref<TextureCubemap> cubemap,
                            Ref<Shader> shader,
                            Ref<UniformBuffer> cameraUBO,
                            Ref<UniformBuffer> skyUBO)
    {
        OLO_PROFILE_FUNCTION();

        if (!cubemap || !shader || !cameraUBO || !skyUBO)
        {
            OLO_CORE_ERROR("SkyBake::RenderSkyToCubemap: null argument");
            return false;
        }

        const u32 face = cubemap->GetWidth();
        const auto& mats = GetCaptureMatrices();

        const bool wasStencilEnabled = RenderCommand::IsStencilTestEnabled();
        if (wasStencilEnabled)
            RenderCommand::DisableStencilTest();

        shader->Bind();
        skyUBO->Bind();

        FramebufferSpecification fbSpec;
        fbSpec.Width = face;
        fbSpec.Height = face;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
        auto framebuffer = Framebuffer::Create(fbSpec);
        if (!framebuffer)
        {
            OLO_CORE_ERROR("SkyBake::RenderSkyToCubemap: failed to allocate framebuffer");
            if (wasStencilEnabled)
                RenderCommand::EnableStencilTest();
            return false;
        }

        auto cubeMesh = MeshPrimitives::CreateSkyboxCube();
        if (!cubeMesh)
        {
            if (wasStencilEnabled)
                RenderCommand::EnableStencilTest();
            return false;
        }

        for (u32 i = 0; i < 6; ++i)
        {
            OLO_PROFILE_SCOPE("SkyBake::Face");

            // Update CameraUBO with this face's view/projection. The shader's
            // vertex stage picks up u_ViewProjection just like every other
            // skybox-style shader, turning the cube vertex into a sample dir.
            ShaderBindingLayout::CameraUBO data;
            data.ViewProjection = mats.Projection * mats.Views[i];
            data.View = mats.Views[i];
            data.Projection = mats.Projection;
            data.Position = glm::vec3(0.0f);
            data._padding0 = 0.0f;
            cameraUBO->SetData(&data, ShaderBindingLayout::CameraUBO::GetSize());
            cameraUBO->Bind();

            framebuffer->Bind();
            RenderCommand::SetViewport(0, 0, face, face);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::ClearColorAndDepth();

            auto vao = cubeMesh->GetVertexArray();
            vao->Bind();
            RenderCommand::DrawIndexed(vao);

            const u32 fbColor = framebuffer->GetColorAttachmentRendererID(0);
            RenderCommand::CopyImageSubDataFull(
                fbColor, RendererAPI::TextureTargetType::Texture2D, 0, 0,
                cubemap->GetRendererID(), RendererAPI::TextureTargetType::TextureCubeMap, 0, static_cast<i32>(i),
                face, face);
        }

        framebuffer->Unbind();

        if (wasStencilEnabled)
            RenderCommand::EnableStencilTest();

        return true;
    }
} // namespace OloEngine::SkyBake
